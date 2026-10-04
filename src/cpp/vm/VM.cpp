#include "VM.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <bit>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <limits>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include "../Error.hpp"
#include "../ir/OpCode.hpp"
#include "../backend/Disasm.hpp"

namespace { struct RuntimeUnwind {}; }

namespace {
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket INVALID_NATIVE_SOCKET = INVALID_SOCKET;
int nativeSocketError() { return WSAGetLastError(); }
void closeNativeSocket(NativeSocket socket) { closesocket(socket); }
std::string nativeSocketErrorText(int code) {
	char* message = nullptr;
	const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(code), 0,
		reinterpret_cast<char*>(&message), 0, nullptr);
	std::string result = size && message ? std::string(message, size) : "socket error " + std::to_string(code);
	if (message) LocalFree(message);
	while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
	return result;
}
bool initializeSockets(std::string& error) {
	static const int startupError = [] {
		WSADATA data{};
		return WSAStartup(MAKEWORD(2, 2), &data);
	}();
	if (startupError != 0) error = nativeSocketErrorText(startupError);
	return startupError == 0;
}
#else
using NativeSocket = int;
constexpr NativeSocket INVALID_NATIVE_SOCKET = -1;
int nativeSocketError() { return errno; }
void closeNativeSocket(NativeSocket socket) { ::close(socket); }
std::string nativeSocketErrorText(int code) { return std::strerror(code); }
bool initializeSockets(std::string&) { return true; }
#endif

NativeSocket decodeSocket(int64_t value) { return static_cast<NativeSocket>(value); }
int64_t encodeSocket(NativeSocket value) { return static_cast<int64_t>(value); }
}

// The opcode-name table moved to backend/Disasm.cpp (opcodeName) so the VM, its
// panic reports and --dump-bytecode all read from one place.

#define vm_panic(msg) panic(msg)
#define runtime_error(msg) raiseRuntime(msg)


VM::VM(Chunk chunk, bool trace, GCMode gcMode, uint32_t heapSlots)
		: m_chunk(std::move(chunk)), m_flags(0), m_trace(trace) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    m_runtimeStrings = m_chunk.strings;
	if (gcMode == GCMode::AARCLocal) {
		m_heap.reserveSlots(heapSlots ? heapSlots : (1u << 20));
	}
	m_gcOwned = makeGCBackend(gcMode, m_heap);
	m_gc = m_gcOwned.get();
	std::fill(std::begin(m_regs), std::end(m_regs), 0);
	std::fill(std::begin(m_rtag), std::end(m_rtag), 0);
	std::fill(std::begin(m_globals), std::end(m_globals), 0);
	std::fill(std::begin(m_globalTags), std::end(m_globalTags), 0);
}

VM::~VM() {
	for (const int64_t handle : m_openSockets) closeNativeSocket(decodeSocket(handle));
}

// Acquire before release.  The reverse order is not equivalent: if the value
// being installed is reachable only through the reference being dropped (a
// register spill restored after a call, or an object loaded out of a dying
// object's field), remove-then-add lets the count hit zero in between and the
// collector reclaims the object we were about to root -- which then comes back
// as a dangling handle.
void VM::setReg(uint16_t reg, int64_t value, uint8_t tag) {
	const uint8_t oldTag = m_rtag[reg];
	const int64_t oldValue = m_regs[reg];
	if (oldTag == TAG_HEAP && tag == TAG_HEAP && oldValue == value) return;
	if (tag == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(value));
	m_regs[reg] = value;
	m_rtag[reg] = tag;
	if (oldTag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(oldValue));
}

void VM::setGlobal(uint8_t slot, int64_t value, uint8_t tag) {
	const uint8_t oldTag = m_globalTags[slot];
	const int64_t oldValue = m_globals[slot];
	if (oldTag == TAG_HEAP && tag == TAG_HEAP && oldValue == value) return;
	if (tag == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(value));
	m_globals[slot] = value;
	m_globalTags[slot] = tag;
	if (oldTag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(oldValue));
}

void VM::finishGC() {
	for (uint16_t r = 0; r < 4096; r++) setReg(r, 0, TAG_INT);
	for (uint16_t g = 0; g < 256; g++) setGlobal(static_cast<uint8_t>(g), 0, TAG_INT);
	while (!m_dataStack.empty()) {
		auto [reg, val, tag] = m_dataStack.top();
		m_dataStack.pop();
		if (tag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(val));
	}
	m_gc->shutdown();
}

int VM::run(int maxExecuteCount) {
	// Build JIT on first run, then execute via JIT
	if (!m_jitReady) {
		if (!tryBuildJit()) {
			vm_panic("failed to decode bytecode");
		}
	}
	return runJit(maxExecuteCount);
}

// ---- JIT Implementation ----

// Decoding lives in backend/Disasm.cpp so `--dump-bytecode` and the JIT agree
// on operand lengths by construction; this only reshapes the result into the
// instruction array the dispatch loop indexes.
bool VM::tryBuildJit() {
	if (m_jitReady) return true;

	m_jitCode.clear();
	m_jitIpMap.clear();

	std::vector<DecodedInstr> decoded;
	std::string decodeError;
	if (!decodeBytecode(m_chunk, decoded, &decodeError)) {
		if (m_trace) std::cerr << decodeError << std::endl;
		return false;
	}

	m_jitCode.reserve(decoded.size());
	for (const DecodedInstr &src : decoded) {
		JitInstruction instr;
		instr.op = src.op;
		instr.byteOffset = src.byteOffset;
		instr.operands.reserve(src.operands.size());
		for (const DecodedOperand &operand : src.operands) {
			JitOperand dst;
			dst.tag = operand.tag;
			dst.intVal = operand.intVal;
			dst.strIdx = operand.strIdx;
			instr.operands.push_back(dst);
		}
		m_jitIpMap[src.byteOffset] = m_jitCode.size();
		m_jitCode.push_back(std::move(instr));
	}

	m_jitReady = true;
	return true;
}

bool VM::resolveJitTarget(uint32_t byteOffset, size_t &target) const {
	auto it = m_jitIpMap.find(byteOffset);
	if (it == m_jitIpMap.end()) return false;
	target = it->second;
	return true;
}

int64_t VM::readIntOperand(const JitOperand &operand) const {
	if (operand.tag == OperandTag::Reg) {
		return m_regs[static_cast<uint16_t>(operand.intVal)];
	}
	if (operand.tag == OperandTag::Imm64) {
		return operand.intVal;
	}
	vm_panic("numeric operand has an invalid tag");
}

std::string VM::readStringOperand(const JitOperand &operand) const {
	if (operand.tag == OperandTag::StrIdx) {
		if (operand.strIdx < m_chunk.strings.size()) {
			return m_chunk.strings[operand.strIdx];
		}
	} else if (operand.tag == OperandTag::Reg) {
		uint16_t reg = static_cast<uint16_t>(operand.intVal);
		if (m_rtag[reg] == 1) {
			uint32_t strIdx = static_cast<uint32_t>(m_regs[reg]);
			if (strIdx < m_runtimeStrings.size()) {
				return m_runtimeStrings[strIdx];
			}
			if (strIdx < m_chunk.strings.size()) {
				return m_chunk.strings[strIdx];
			}
		}
	}
	vm_panic("string operand has an invalid tag or string index");
}

uint32_t VM::internRuntimeString(const std::string& s) {
	uint32_t idx = static_cast<uint32_t>(m_runtimeStrings.size());
	m_runtimeStrings.push_back(s);
	return idx;
}

void VM::pushCallFrame(size_t returnIp, size_t callIp) {
	CallFrame frame{returnIp, callIp, {}};
	uint16_t maximum = 15;
	if (callIp < m_jitCode.size()) {
		const uint32_t offset = m_jitCode[callIp].byteOffset;
		for (const auto& function : m_chunk.functions) {
			if (offset >= function.start && offset < function.end) {
				maximum = function.maxLocalRegister;
				break;
			}
		}
	}
	frame.locals.reserve(maximum >= 16 ? maximum - 15 : 0);
	for (uint16_t reg = 16; reg <= maximum; ++reg) {
		if (reg >= 240 && reg <= 255) continue;
		if (m_rtag[reg] == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(m_regs[reg]));
		frame.locals.push_back(SavedRegister{reg, m_regs[reg], m_rtag[reg]});
	}
	m_callStack.push(std::move(frame));
}

void VM::restoreCallFrame() {
	if (m_callStack.empty()) panic("attempted to restore an empty call stack");
	auto frame = std::move(m_callStack.top());
	m_callStack.pop();
	for (uint16_t reg = 1; reg <= 10; ++reg) setReg(reg, 0, TAG_INT);
	for (uint16_t reg = 240; reg <= 255; ++reg) setReg(reg, 0, TAG_INT);
	for (const auto& saved : frame.locals) {
		setReg(saved.reg, saved.value, saved.tag);
		if (saved.tag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(saved.value));
	}
}

[[noreturn]] void VM::panic(const std::string& message) const {
	std::ostringstream out;
	out << "Loxis VM panic: " << message;
	if (m_jitIp < m_jitCode.size()) {
		out << "\n  opcode: " << opcodeName(m_jitCode[m_jitIp].op)
		    << "\n  bytecode offset: 0x" << std::hex << m_jitCode[m_jitIp].byteOffset;
	}
	out << "\nThis is a compiler or VM bug.";
	throw RuntimeError(out.str());
}

void VM::raiseRuntime(const std::string& message) {
	uint32_t typeId = ~0u;
	for (uint32_t i = 0; i < m_chunk.typeNames.size(); ++i) {
		if (m_chunk.typeNames[i] == "RuntimeException") { typeId = i; break; }
	}
	if (typeId == ~0u) panic("RuntimeException type metadata is missing");
	const uint32_t handle = m_heap.alloc(Heap::SLOT_SIZE, typeId);
	if (handle == Heap::INVALID) panic("heap exhausted while creating RuntimeException");
	m_gc->onAlloc(handle);
	const uint32_t stringId = internRuntimeString(message);
	if (!m_heap.storeCell(handle, 0, stringId, TAG_STR))
		panic("failed to initialize RuntimeException.message");
	throwValue(handle, TAG_HEAP);
	throw RuntimeUnwind{};
}

void VM::throwValue(int64_t value, uint8_t tag) {
	if (!m_exceptionHandlers.empty()) {
		auto handler = m_exceptionHandlers.back();
		m_exceptionHandlers.pop_back();
		if (tag == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(value));
		while (m_callStack.size() > handler.callDepth) restoreCallFrame();
		while (m_dataStack.size() > handler.dataDepth) {
			auto [savedReg, saved, savedTag] = m_dataStack.top();
			m_dataStack.pop();
			setReg(savedReg, saved, savedTag);
			if (savedTag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(saved));
		}
		setReg(handler.exceptionReg, value, tag);
		if (tag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(value));
		m_jitIp = handler.targetIp;
		return;
	}

	std::string typeName = "Exception";
	std::string message;
	if (tag == TAG_HEAP) {
		const uint32_t handle = static_cast<uint32_t>(value);
		if (!m_heap.valid(handle)) panic("throw received an invalid exception handle");
		const uint32_t typeId = m_heap.getTypeId(handle);
		if (typeId < m_chunk.typeNames.size() && !m_chunk.typeNames[typeId].empty())
			typeName = m_chunk.typeNames[typeId];
		auto messageCell = m_heap.loadCell(handle, 0);
		if (!messageCell) panic("exception object has no readable message field");
		auto [rawMessage, messageTag] = *messageCell;
		if (messageTag == TAG_STR) {
			const uint32_t index = static_cast<uint32_t>(rawMessage);
			if (index < m_runtimeStrings.size()) message = m_runtimeStrings[index];
			else if (index < m_chunk.strings.size()) message = m_chunk.strings[index];
			else panic("exception message contains an invalid string index");
		}
	}
	auto functionAt = [&](size_t jitIp) -> std::string {
		if (jitIp >= m_jitCode.size()) return "<unknown>";
		const uint32_t byteOffset = m_jitCode[jitIp].byteOffset;
		for (const auto& fn : m_chunk.functions)
			if (byteOffset >= fn.start && byteOffset < fn.end) return fn.name;
		return "<unknown>";
	};
	auto sourceAt = [&](size_t jitIp) -> const SourceMapEntry* {
		if (jitIp >= m_jitCode.size()) return nullptr;
		const uint32_t offset = m_jitCode[jitIp].byteOffset;
		uint32_t functionStart = 0;
		for (const auto& fn : m_chunk.functions) {
			if (offset >= fn.start && offset < fn.end) { functionStart = fn.start; break; }
		}
		for (auto it = m_chunk.sourceMap.rbegin(); it != m_chunk.sourceMap.rend(); ++it) {
			if (it->offset > offset) continue;
			if (it->offset < functionStart) break;
			return &*it;
		}
		return nullptr;
	};
	auto appendFrame = [&](std::ostringstream& out, size_t ip) {
		const auto* sourceLoc = sourceAt(ip);
		if (!sourceLoc) return;
		out << "  File \"" << sourceLoc->file << "\", line " << sourceLoc->line
		    << ", in " << functionAt(ip) << "\n";
		auto file = m_chunk.sourceLines.find(sourceLoc->file);
		if (file == m_chunk.sourceLines.end() || sourceLoc->line == 0 ||
		    sourceLoc->line > file->second.size()) return;
		const auto& line = file->second[sourceLoc->line - 1];
		out << "    " << line << "\n    ";
		const size_t start = sourceLoc->col > 0 ? sourceLoc->col - 1 : 0;
		for (size_t i = 0; i < start && i < line.size(); ++i)
			out << (line[i] == '\t' ? '\t' : ' ');
		out << std::string(std::max<size_t>(1, line.size() > start ? line.size() - start : 1), '^') << "\n";
	};
	std::ostringstream report;
	report << "Traceback (most recent call last):\n";
	auto calls = m_callStack;
	std::vector<size_t> callerIps;
	while (!calls.empty()) { callerIps.push_back(calls.top().callIp); calls.pop(); }
	for (auto it = callerIps.rbegin(); it != callerIps.rend(); ++it) appendFrame(report, *it);
	appendFrame(report, m_jitIp);
	report << typeName;
	if (!message.empty()) report << ": " << message;
	throw UncaughtLoxisException(report.str());
}

void VM::invokeSyscall(uint8_t op) {
	enum { S_READCHAR=0, S_S2I=1, S_TYPEOF=2, S_CHR=3, S_WRITE=10, S_PRINT=11,
	       S_HALLOC=20, S_HREAD=21, S_HSET=22, S_HFREE=23,
	       S_STRLEN=30, S_I2S=31, S_STRCHAR=32, S_STRSLICE=33, S_STRINDEXOF=34,
	       S_FILEREAD=40, S_FILEWRITE=41, S_FILEAPPEND=42, S_FILEREMOVE=43,
	       S_ERROR=99 };
	auto regString = [&](uint8_t reg) -> std::string {
		uint32_t si=static_cast<uint32_t>(m_regs[reg]);
		if(si<m_runtimeStrings.size())return m_runtimeStrings[si];
		if(si<m_chunk.strings.size())return m_chunk.strings[si];
		vm_panic("syscall " + std::to_string(op) + " received invalid string index " +
		              std::to_string(si) + " (tag " + std::to_string(m_rtag[reg]) + ")");
	};
	switch (op) {
	case S_READCHAR: { int c=std::cin.get(); setReg(0, c, TAG_INT); break; }
	case S_S2I: {
		std::string s=regString(1);
		int64_t parsed=0;
		try {
			size_t consumed = 0;
			parsed = std::stoll(s, &consumed, 10);
			if (consumed != s.size()) runtime_error("invalid integer: '" + s + "'");
		} catch (const std::invalid_argument&) {
			runtime_error("invalid integer: '" + s + "'");
		} catch (const std::out_of_range&) {
			runtime_error("integer out of range: '" + s + "'");
		}
		setReg(0, parsed, TAG_INT); break; }
	case S_TYPEOF: { setReg(0, m_heap.getTypeId(static_cast<uint32_t>(m_regs[1])), TAG_INT); break; }
		case S_CHR: { setReg(0, internRuntimeString(std::string(1,static_cast<char>(static_cast<int>(m_regs[1])))), TAG_STR); break; }
	case S_WRITE: {
		int64_t v=m_regs[1];
		if(m_rtag[1]==0)std::cout<<v;
		else if(m_rtag[1]==1){uint32_t si=static_cast<uint32_t>(v);
		if(si<m_runtimeStrings.size())std::cout<<m_runtimeStrings[si];
		else if(si<m_chunk.strings.size())std::cout<<m_chunk.strings[si];
		else vm_panic("write received an invalid string index");}
		else vm_panic("write received an invalid runtime tag " + std::to_string(m_rtag[1])); break; }
	case S_PRINT: {
		int64_t v=m_regs[1];
		if(m_rtag[1]==0)std::cout<<v<<"\n";
		else if(m_rtag[1]==1){uint32_t si=static_cast<uint32_t>(v);
		if(si<m_runtimeStrings.size())std::cout<<m_runtimeStrings[si]<<"\n";
		else if(si<m_chunk.strings.size())std::cout<<m_chunk.strings[si]<<"\n";
		else vm_panic("print received an invalid string index");}
		else vm_panic("print received an invalid runtime tag " + std::to_string(m_rtag[1])); break; }
	case S_HALLOC: {
		uint32_t h=m_heap.alloc(static_cast<uint32_t>(m_regs[1]),0);
		if (h==Heap::INVALID) runtime_error("heap exhausted (slot space full)");
		m_gc->onAlloc(h); setReg(0, h, TAG_HEAP); break; }
	case S_HREAD: {
		auto c=m_heap.loadCell(static_cast<uint32_t>(m_regs[1]), static_cast<uint32_t>(m_regs[2]));
		if (!c) runtime_error("heap read out of bounds or through an invalid handle");
		setReg(0, c->first, c->second); break; }
	case S_HSET: {
		uint32_t base=static_cast<uint32_t>(m_regs[1]);
		uint32_t off=static_cast<uint32_t>(m_regs[2]);
		auto old=m_heap.loadCell(base, off);
		if (!old) runtime_error("heap write out of bounds or through an invalid handle");
		const uint8_t newTag=m_rtag[3];
		if (old->second==TAG_HEAP && newTag==TAG_HEAP && old->first==m_regs[3]) break;
		if (old->second==TAG_HEAP) m_gc->edgeRemove(base, static_cast<uint32_t>(old->first));
		if (!m_heap.storeCell(base, off, m_regs[3], newTag)) vm_panic("validated heap write failed");
		if (newTag==TAG_HEAP) m_gc->edgeAdd(base, static_cast<uint32_t>(m_regs[3]));
		break; }
	case S_HFREE: { m_gc->onExplicitFree(static_cast<uint32_t>(m_regs[1])); break; }
	case S_STRLEN: {
		int64_t len=static_cast<int64_t>(regString(1).size());
		setReg(0, len, TAG_INT); break; }
	case S_I2S: { setReg(0, internRuntimeString(std::to_string(m_regs[1])), TAG_STR); break; }
	case S_STRCHAR: {
		std::string s=regString(1);
		int64_t idx=m_regs[2];
		setReg(0, (idx>=0 && static_cast<size_t>(idx)<s.size()) ? static_cast<unsigned char>(s[static_cast<size_t>(idx)]) : -1, TAG_INT);
		break; }
	case S_STRSLICE: {
		std::string s=regString(1);
		int64_t start=m_regs[2], count=m_regs[3];
		if(start<0)start=0;
		if(count<0)count=0;
		size_t begin=static_cast<size_t>(start);
		if(begin>s.size())begin=s.size();
		size_t n=static_cast<size_t>(count);
		if(begin+n>s.size())n=s.size()-begin;
		setReg(0, internRuntimeString(s.substr(begin,n)), TAG_STR); break; }
	case S_STRINDEXOF: {
		std::string h=regString(1);
		std::string needle=regString(2);
		auto pos=h.find(needle);
		setReg(0, (pos==std::string::npos) ? -1 : static_cast<int64_t>(pos), TAG_INT);
		break; }
    case 44: case 47: case 48: case 49: {
        std::error_code ec;
        auto path = std::filesystem::u8path(regString(1));
        bool ok = false;
        if (op == 44) ok = std::filesystem::exists(path, ec);
        if (op == 47) ok = std::filesystem::is_directory(path, ec);
        if (op == 48) {
            ok = std::filesystem::create_directories(path, ec);
            if (!ec && !ok) ok = std::filesystem::is_directory(path, ec);
        }
        if (op == 49) {
            if (std::filesystem::is_directory(path, ec) && !ec)
                ok = std::filesystem::remove(path, ec);
            else if (!ec) ec = std::make_error_code(std::errc::not_a_directory);
        }
        m_ioError = ec ? ec.message() : "";
        setReg(0, ok && !ec ? 1 : 0, TAG_INT); break;
    }
    case 45: {
        const auto path = std::filesystem::u8path(regString(1));
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            m_ioError = "cannot read directory as file"; setReg(0, 0, TAG_NULL); break;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) { m_ioError = "cannot open file: " + regString(1); setReg(0, 0, TAG_NULL); break; }
        std::string content;
        char buffer[8192];
        while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
            content.append(buffer, static_cast<size_t>(in.gcount()));
        if (in.bad() || !in.eof()) {
            m_ioError = "cannot read file: " + regString(1); setReg(0, 0, TAG_NULL); break;
        }
        m_ioError.clear();
        setReg(0, internRuntimeString(content), TAG_STR); break;
    }
    case 46: setReg(0, internRuntimeString(m_ioError), TAG_STR); break;
    case 50: case 51: case 52: case 53: case 54: {
        auto path = std::filesystem::u8path(regString(1));
        if (op == 50) path /= std::filesystem::u8path(regString(2));
        if (op == 50 || op == 51) path = path.lexically_normal();
        if (op == 52) path = path.parent_path();
        if (op == 53) path = path.filename();
        if (op == 54) path = path.extension();
        auto utf8 = path.generic_u8string();
        setReg(0, internRuntimeString(std::string(utf8.begin(), utf8.end())), TAG_STR); break;
    }
    case 55: {
        auto utf8 = std::filesystem::current_path().generic_u8string();
        setReg(0, internRuntimeString(std::string(utf8.begin(), utf8.end())), TAG_STR); break;
    }
    case 56: {
        m_directoryEntries.clear();
        std::error_code ec;
        auto it = std::filesystem::directory_iterator(std::filesystem::u8path(regString(1)), ec);
        const auto end = std::filesystem::directory_iterator();
        while (!ec && it != end) {
            auto utf8 = it->path().filename().generic_u8string();
            m_directoryEntries.emplace_back(utf8.begin(), utf8.end());
            it.increment(ec);
        }
        m_ioError = ec ? ec.message() : "";
        std::sort(m_directoryEntries.begin(), m_directoryEntries.end());
        setReg(0, ec ? -1 : static_cast<int64_t>(m_directoryEntries.size()), TAG_INT); break;
    }
    case 57: {
        auto i = m_regs[1];
        if (i < 0 || static_cast<size_t>(i) >= m_directoryEntries.size()) runtime_error("directory entry index out of bounds");
        setReg(0, internRuntimeString(m_directoryEntries[static_cast<size_t>(i)]), TAG_STR); break;
    }
    case 60: {
        auto name = regString(1);
        const char* value = std::getenv(name.c_str());
        if (value) setReg(0, internRuntimeString(value), TAG_STR);
        else setReg(0, 0, TAG_NULL);
        break;
    }
    case 61: setReg(0, static_cast<int64_t>(m_arguments.size()), TAG_INT); break;
    case 62: {
        auto i = m_regs[1];
        if (i < 0 || static_cast<size_t>(i) >= m_arguments.size()) runtime_error("argument index out of bounds");
        setReg(0, internRuntimeString(m_arguments[static_cast<size_t>(i)]), TAG_STR); break;
    }
    case 63: setReg(0, std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(), TAG_INT); break;
    case 64: setReg(0, std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(), TAG_INT); break;
    case 70: case 71: case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79: case 80: {
        double x = m_rtag[1] == TAG_FLOAT ? std::bit_cast<double>(m_regs[1]) : static_cast<double>(m_regs[1]);
        double y = m_rtag[2] == TAG_FLOAT ? std::bit_cast<double>(m_regs[2]) : static_cast<double>(m_regs[2]);
        if (op == 79) { setReg(0, std::isfinite(x) ? 1 : 0, TAG_INT); break; }
        if (op == 80) {
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::setprecision(std::numeric_limits<double>::max_digits10) << x;
            setReg(0, internRuntimeString(out.str()), TAG_STR); break;
        }
        double value = x;
        if (op == 70) value = std::sqrt(x);
        if (op == 71) value = std::floor(x);
        if (op == 72) value = std::ceil(x);
        if (op == 73) value = std::round(x);
        if (op == 74) value = std::sin(x);
        if (op == 75) value = std::cos(x);
        if (op == 76) value = std::log(x);
        if (op == 77) value = std::exp(x);
        if (op == 78) value = std::pow(x, y);
        setReg(0, std::bit_cast<int64_t>(value), TAG_FLOAT); break;
    }
    case 81: case 82: {
        const auto text = regString(1);
        std::istringstream input(text);
        input.imbue(std::locale::classic());
        input >> std::noskipws;
        double value = 0;
        input >> value;
        bool valid = !input.fail() && input.eof() && std::isfinite(value);
        if (op == 82) setReg(0, valid ? 1 : 0, TAG_INT);
        else {
            if (!valid) runtime_error("invalid floating-point number");
            setReg(0, std::bit_cast<int64_t>(value), TAG_FLOAT);
        }
        break;
    }
    case 83: case 84: {
        m_netError.clear();
        if (!initializeSockets(m_netError)) { setReg(0, -1, TAG_INT); break; }
        const std::string host = regString(1);
        const int64_t port = m_regs[2];
        if (port < 0 || port > 65535) {
            m_netError = "port must be between 0 and 65535";
            setReg(0, -1, TAG_INT); break;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (op == 84) hints.ai_flags = AI_PASSIVE;
        addrinfo* addresses = nullptr;
        const std::string service = std::to_string(port);
        const char* hostName = op == 84 && host.empty() ? nullptr : host.c_str();
        const int lookup = getaddrinfo(hostName, service.c_str(), &hints, &addresses);
        if (lookup != 0) {
#ifdef _WIN32
            m_netError = gai_strerrorA(lookup);
#else
            m_netError = gai_strerror(lookup);
#endif
            setReg(0, -1, TAG_INT); break;
        }
        NativeSocket opened = INVALID_NATIVE_SOCKET;
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            NativeSocket candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == INVALID_NATIVE_SOCKET) continue;
            if (op == 84) {
                int reuse = 1;
                setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR,
                    reinterpret_cast<const char*>(&reuse), static_cast<socklen_t>(sizeof(reuse)));
                if (::bind(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0 &&
                    ::listen(candidate, SOMAXCONN) == 0) {
                    opened = candidate; break;
                }
            } else if (::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
                opened = candidate; break;
            }
            m_netError = nativeSocketErrorText(nativeSocketError());
            closeNativeSocket(candidate);
        }
        freeaddrinfo(addresses);
        if (opened == INVALID_NATIVE_SOCKET) {
            if (m_netError.empty()) m_netError = op == 84 ? "cannot bind TCP listener" : "cannot connect TCP socket";
            setReg(0, -1, TAG_INT); break;
        }
        const int64_t handle = encodeSocket(opened);
        m_openSockets.insert(handle);
        setReg(0, handle, TAG_INT); break;
    }
    case 85: {
        const int64_t listenerHandle = m_regs[1];
        if (!m_openSockets.contains(listenerHandle)) {
            m_netError = "invalid or closed socket"; setReg(0, -1, TAG_INT); break;
        }
        NativeSocket accepted = ::accept(decodeSocket(listenerHandle), nullptr, nullptr);
        if (accepted == INVALID_NATIVE_SOCKET) {
            m_netError = nativeSocketErrorText(nativeSocketError()); setReg(0, -1, TAG_INT); break;
        }
        const int64_t handle = encodeSocket(accepted);
        m_openSockets.insert(handle);
        m_netError.clear();
        setReg(0, handle, TAG_INT); break;
    }
    case 86: {
        const int64_t handle = m_regs[1];
        const int64_t maximum = m_regs[2];
        if (!m_openSockets.contains(handle)) {
            m_netError = "invalid or closed socket"; setReg(0, 0, TAG_NULL); break;
        }
        if (maximum <= 0 || maximum > 16 * 1024 * 1024) {
            m_netError = "read size must be between 1 and 16777216"; setReg(0, 0, TAG_NULL); break;
        }
        std::string data(static_cast<size_t>(maximum), '\0');
        const int received = ::recv(decodeSocket(handle), data.data(), static_cast<int>(data.size()), 0);
        if (received < 0) {
            m_netError = nativeSocketErrorText(nativeSocketError()); setReg(0, 0, TAG_NULL); break;
        }
        data.resize(static_cast<size_t>(received));
        m_netError.clear();
        setReg(0, internRuntimeString(data), TAG_STR); break;
    }
    case 87: {
        const int64_t handle = m_regs[1];
        const std::string data = regString(2);
        if (!m_openSockets.contains(handle)) {
            m_netError = "invalid or closed socket"; setReg(0, -1, TAG_INT); break;
        }
        size_t written = 0;
        while (written < data.size()) {
#ifdef MSG_NOSIGNAL
            constexpr int sendFlags = MSG_NOSIGNAL;
#else
            constexpr int sendFlags = 0;
#endif
            const int count = ::send(decodeSocket(handle), data.data() + written,
                static_cast<int>(std::min<size_t>(data.size() - written, static_cast<size_t>(std::numeric_limits<int>::max()))),
                sendFlags);
            if (count <= 0) {
                m_netError = nativeSocketErrorText(nativeSocketError()); setReg(0, -1, TAG_INT); break;
            }
            written += static_cast<size_t>(count);
        }
        if (written == data.size()) {
            m_netError.clear();
            setReg(0, static_cast<int64_t>(written), TAG_INT);
        }
        break;
    }
    case 88: {
        const int64_t handle = m_regs[1];
        const auto found = m_openSockets.find(handle);
        if (found == m_openSockets.end()) {
            m_netError = "invalid or closed socket"; setReg(0, 0, TAG_INT); break;
        }
        closeNativeSocket(decodeSocket(handle));
        m_openSockets.erase(found);
        m_netError.clear();
        setReg(0, 1, TAG_INT); break;
    }
    case 89: setReg(0, internRuntimeString(m_netError), TAG_STR); break;
    case 90: {
        const int64_t handle = m_regs[1];
        if (!m_openSockets.contains(handle)) {
            m_netError = "invalid or closed socket"; setReg(0, -1, TAG_INT); break;
        }
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (getsockname(decodeSocket(handle), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            m_netError = nativeSocketErrorText(nativeSocketError()); setReg(0, -1, TAG_INT); break;
        }
        int port = address.ss_family == AF_INET
            ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port)
            : ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
        m_netError.clear();
        setReg(0, port, TAG_INT); break;
    }
	case S_FILEREAD: {
		std::ifstream in(std::filesystem::u8path(regString(1)), std::ios::binary);
		if(!in){setReg(0, internRuntimeString(""), TAG_STR); break;}
		std::ostringstream ss; ss<<in.rdbuf();
		setReg(0, internRuntimeString(ss.str()), TAG_STR); break; }
	case S_FILEWRITE: {
		std::ofstream out(std::filesystem::u8path(regString(1)), std::ios::binary | std::ios::trunc);
		if(!out){setReg(0, 0, TAG_INT); break;}
		out<<regString(2);
		out.close();
		setReg(0, out.good()?1:0, TAG_INT); break; }
	case S_FILEAPPEND: {
		std::ofstream out(std::filesystem::u8path(regString(1)), std::ios::binary | std::ios::app);
		if(!out){setReg(0, 0, TAG_INT); break;}
		out<<regString(2);
		out.close();
		setReg(0, out.good()?1:0, TAG_INT); break; }
	case S_FILEREMOVE: {
        std::error_code ec;
        const auto path = std::filesystem::u8path(regString(1));
        bool ok = false;
        if (!std::filesystem::is_directory(path, ec) && !ec) ok = std::filesystem::remove(path, ec);
        setReg(0, ok && !ec ? 1 : 0, TAG_INT); break; }
	case S_ERROR: {
        runtime_error(regString(1));
        break; }
	default: vm_panic("unknown syscall " + std::to_string(op));
	}
}

void VM::execJitInstruction(const JitInstruction &instr, bool &halted) {
	switch (instr.op) {
		case OpCode::Nop:
			m_jitIp++;
			break;

		case OpCode::TryPush: {
			size_t target = 0;
			if (!resolveJitTarget(static_cast<uint32_t>(instr.operands[0].intVal), target))
				vm_panic("invalid exception handler target");
			m_exceptionHandlers.push_back(ExceptionHandler{
				target,
				static_cast<uint16_t>(instr.operands[1].intVal),
				m_callStack.size(),
				m_dataStack.size()
			});
			m_jitIp++;
			break;
		}

		case OpCode::TryPop:
			if (m_exceptionHandlers.empty()) vm_panic("exception handler stack underflow");
			m_exceptionHandlers.pop_back();
			m_jitIp++;
			break;

		case OpCode::Throw: {
			const uint16_t source = static_cast<uint16_t>(instr.operands[0].intVal);
			throwValue(m_regs[source], m_rtag[source]);
			break;
		}
		case OpCode::IsA: {
			uint16_t source = static_cast<uint16_t>(instr.operands[0].intVal);
			uint32_t targetType = static_cast<uint32_t>(readIntOperand(instr.operands[1]));
			uint16_t result = static_cast<uint16_t>(instr.operands[2].intVal);
			bool matches = false;
			if (m_rtag[source] == TAG_HEAP) {
				const uint32_t handle = static_cast<uint32_t>(m_regs[source]);
				if (!m_heap.valid(handle)) vm_panic("type check received an invalid heap handle");
				uint32_t type = m_heap.getTypeId(handle);
				while (type != ~0u) {
					if (type == targetType) { matches = true; break; }
					if (type >= m_chunk.typeParents.size()) break;
					type = m_chunk.typeParents[type];
				}
			}
			setReg(result, matches ? 1 : 0, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Push: {
			auto &dst = instr.operands[0];
			auto &val = instr.operands[1];
			uint16_t dstReg = static_cast<uint16_t>(dst.intVal);
			if (val.tag == OperandTag::Reg) {
				uint16_t srcReg = static_cast<uint16_t>(val.intVal);
				setReg(dstReg, m_regs[srcReg], m_rtag[srcReg]);
			} else if (val.tag == OperandTag::Imm64) {
				setReg(dstReg, val.intVal, TAG_INT);
            } else if (val.tag == OperandTag::Null) {
                setReg(dstReg, 0, TAG_NULL);
            } else if (val.tag == OperandTag::Float64) {
                setReg(dstReg, val.intVal, TAG_FLOAT);
			} else if (val.tag == OperandTag::StrIdx) {
				setReg(dstReg, val.strIdx, TAG_STR);
			} else if (val.tag == OperandTag::Addr) {
				setReg(dstReg, val.intVal, TAG_INT);
			}
			m_jitIp++;
			break;
		}

		case OpCode::Pop:
			m_jitIp++;
			break;

		case OpCode::PushR: {
			auto &reg = instr.operands[0];
			uint16_t r = static_cast<uint16_t>(reg.intVal);
			// The data stack is a root set too: the register keeps its own
			// reference, and the saved copy adds a second one.
			if (m_rtag[r] == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(m_regs[r]));
			m_dataStack.push({r, m_regs[r], m_rtag[r]});
			m_jitIp++;
			break;
		}

		case OpCode::PopR: {
			auto &reg = instr.operands[0];
			uint16_t r = static_cast<uint16_t>(reg.intVal);
			if (!m_dataStack.empty()) {
				auto [savedReg, val, tag] = m_dataStack.top();
				m_dataStack.pop();
				if (savedReg != r) vm_panic("register spill stack mismatch");
				// Install into the register first (acquire), then drop the
				// spill-slot reference (release).  A callee may have clobbered
				// this register, so the spill slot can be the object's only
				// reference -- releasing it first would reclaim it.
				setReg(r, val, tag);
				if (tag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(val));
			}
			m_jitIp++;
			break;
		}

		case OpCode::Alloc: {
			// operands[0] = type_id (Imm64), [1] = size_reg, [2] = result_reg
			uint32_t typeId = static_cast<uint32_t>(instr.operands[0].intVal);
			uint32_t size = static_cast<uint32_t>(readIntOperand(instr.operands[1]));
			uint16_t resReg = static_cast<uint16_t>(instr.operands[2].intVal);
			uint32_t handle = m_heap.alloc(size, typeId);
			if (handle == Heap::INVALID) runtime_error("heap exhausted (slot space full)");
			m_gc->onAlloc(handle);
			setReg(resReg, static_cast<int64_t>(handle), TAG_HEAP);
			m_jitIp++;
			break;
		}

		case OpCode::HFree: {
			auto &baseOp = instr.operands[0];
			uint16_t baseReg = static_cast<uint16_t>(baseOp.intVal);
			m_gc->onExplicitFree(static_cast<uint32_t>(m_regs[baseReg]));
			m_jitIp++;
			break;
		}

		case OpCode::HStore: {
			// operands[0]=base_reg, [1]=offset_reg, [2]=value_reg
			auto &baseOp = instr.operands[0];
			auto &offOp = instr.operands[1];
			auto &valOp = instr.operands[2];
			uint32_t base = static_cast<uint32_t>(m_regs[static_cast<uint16_t>(baseOp.intVal)]);
			uint32_t off = static_cast<uint32_t>(m_regs[static_cast<uint16_t>(offOp.intVal)]);
			uint16_t valReg = static_cast<uint16_t>(valOp.intVal);
			// The old cell's own tag decides whether an edge existed — the
			// writing register says nothing about what used to be there.
			auto old = m_heap.loadCell(base, off);
			if (!old) runtime_error("heap write out of bounds or through an invalid handle");
			const uint8_t newTag = m_rtag[valReg];
			if (old->second == TAG_HEAP && newTag == TAG_HEAP && old->first == m_regs[valReg]) {
				m_jitIp++;
				break;
			}
			if (old->second == TAG_HEAP) {
				m_gc->edgeRemove(base, static_cast<uint32_t>(old->first));
			}
			if (!m_heap.storeCell(base, off, m_regs[valReg], newTag)) vm_panic("validated heap write failed");
			if (newTag == TAG_HEAP) {
				m_gc->edgeAdd(base, static_cast<uint32_t>(m_regs[valReg]));
			}
			m_jitIp++;
			break;
		}

		case OpCode::HLoad: {
			// operands[0]=base_reg, [1]=offset_reg, [2]=result_reg
			auto &baseOp = instr.operands[0];
			auto &offOp = instr.operands[1];
			auto &resultOp = instr.operands[2];
			uint32_t base = static_cast<uint32_t>(m_regs[static_cast<uint16_t>(baseOp.intVal)]);
			uint32_t off = static_cast<uint32_t>(m_regs[static_cast<uint16_t>(offOp.intVal)]);
			uint16_t resultReg = static_cast<uint16_t>(resultOp.intVal);
			auto cell = m_heap.loadCell(base, off);
			if (!cell) runtime_error("heap read out of bounds or through an invalid handle");
			auto [val, tag] = *cell;
			setReg(resultReg, val, tag);
			m_jitIp++;
			break;
		}

		case OpCode::Mov: {
			auto &dst = instr.operands[0];
			auto &val = instr.operands[1];
			uint16_t dstReg = static_cast<uint16_t>(dst.intVal);
			if (val.tag == OperandTag::Imm64) {
				setReg(dstReg, val.intVal, TAG_INT);
			} else if (val.tag == OperandTag::Addr) {
				setReg(dstReg, val.intVal, TAG_INT);
			} else if (val.tag == OperandTag::Reg) {
				uint16_t srcReg = static_cast<uint16_t>(val.intVal);
				setReg(dstReg, m_regs[srcReg], m_rtag[srcReg]);
			}
			m_jitIp++;
			break;
		}

		case OpCode::GLoad: {
			uint32_t slot = static_cast<uint32_t>(readIntOperand(instr.operands[0]));
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[1].intVal);
			if (slot >= 256) vm_panic("global slot out of bounds");
			setReg(dstReg, m_globals[slot], m_globalTags[slot]);
			m_jitIp++;
			break;
		}

		case OpCode::GStore: {
			uint32_t slot = static_cast<uint32_t>(readIntOperand(instr.operands[0]));
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[1].intVal);
			if (slot >= 256) vm_panic("global slot out of bounds");
			setGlobal(static_cast<uint8_t>(slot), m_regs[srcReg], m_rtag[srcReg]);
			m_jitIp++;
			break;
		}
		case OpCode::Add: {
			auto &dst = instr.operands[0];
			auto &src = instr.operands[1];
			auto &val = instr.operands[2];
			uint16_t srcReg = static_cast<uint16_t>(src.intVal);
			uint16_t dstReg = static_cast<uint16_t>(dst.intVal);

			// String concatenation: both operands tagged as strings
			if (m_rtag[srcReg] == 1 && val.tag == OperandTag::Reg) {
				uint16_t valReg = static_cast<uint16_t>(val.intVal);
				if (m_rtag[valReg] == 1) {
					std::string lhs = readStringOperand(src);
					std::string rhs = readStringOperand(val);
					setReg(dstReg, static_cast<int64_t>(internRuntimeString(lhs + rhs)), TAG_STR);
					m_jitIp++;
					break;
				}
			}
			int64_t operand = readIntOperand(val);
			setReg(dstReg, m_regs[srcReg] + operand, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Sub: {
			auto &src = instr.operands[0];
			auto &val = instr.operands[1];
			auto &dst = instr.operands[2];
			uint16_t srcReg = static_cast<uint16_t>(src.intVal);
			uint16_t dstReg = static_cast<uint16_t>(dst.intVal);
			int64_t operand = readIntOperand(val);
			setReg(dstReg, m_regs[srcReg] - operand, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Mul: {
			auto &src = instr.operands[0];
			auto &val = instr.operands[1];
			auto &dst = instr.operands[2];
			uint16_t srcReg = static_cast<uint16_t>(src.intVal);
			uint16_t dstReg = static_cast<uint16_t>(dst.intVal);
			int64_t operand = readIntOperand(val);
			setReg(dstReg, m_regs[srcReg] * operand, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Div: {
			auto &src = instr.operands[0];
			auto &val = instr.operands[1];
			auto &dst = instr.operands[2];
			uint16_t srcReg = static_cast<uint16_t>(src.intVal);
			uint16_t dstReg = static_cast<uint16_t>(dst.intVal);
			int64_t operand = readIntOperand(val);
			if (operand == 0) {
				runtime_error("division by zero");
			}
			setReg(dstReg, m_regs[srcReg] / operand, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Shl: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			int64_t shift = readIntOperand(instr.operands[1]);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[2].intVal);
			setReg(dstReg, m_regs[srcReg] << shift, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Shr: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			int64_t shift = readIntOperand(instr.operands[1]);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[2].intVal);
			setReg(dstReg, m_regs[srcReg] >> shift, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Rem: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			int64_t operand = readIntOperand(instr.operands[1]);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[2].intVal);
			if (operand == 0) runtime_error("remainder by zero");
			setReg(dstReg, m_regs[srcReg] % operand, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::BitAnd:
		case OpCode::BitOr:
		case OpCode::BitXor: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			int64_t operand = readIntOperand(instr.operands[1]);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[2].intVal);
			int64_t result;
			if (instr.op == OpCode::BitAnd) result = m_regs[srcReg] & operand;
			else if (instr.op == OpCode::BitOr) result = m_regs[srcReg] | operand;
			else result = m_regs[srcReg] ^ operand;
			setReg(dstReg, result, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::FAdd: {
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[1].intVal);
			uint16_t valReg = static_cast<uint16_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a + b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FSub: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t valReg = static_cast<uint16_t>(instr.operands[1].intVal);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a - b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FMul: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t valReg = static_cast<uint16_t>(instr.operands[1].intVal);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a * b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FDiv: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t valReg = static_cast<uint16_t>(instr.operands[1].intVal);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a / b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::ItoF: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[1].intVal);
			double d = static_cast<double>(m_regs[srcReg]);
			setReg(dstReg, std::bit_cast<int64_t>(d), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FtoI: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[1].intVal);
			double d = std::bit_cast<double>(m_regs[srcReg]);
			setReg(dstReg, static_cast<int64_t>(d), TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::FNeg: {
			uint16_t srcReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t dstReg = static_cast<uint16_t>(instr.operands[1].intVal);
			double d = std::bit_cast<double>(m_regs[srcReg]);
			setReg(dstReg, std::bit_cast<int64_t>(-d), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::Cmp: {
			auto &r0 = instr.operands[0];
			auto &r1 = instr.operands[1];
			uint16_t reg0 = static_cast<uint16_t>(r0.intVal);
			uint16_t reg1 = static_cast<uint16_t>(r1.intVal);
			m_flags = 0;
            if (m_rtag[reg0] == TAG_NULL || m_rtag[reg1] == TAG_NULL) {
                if (m_rtag[reg0] == m_rtag[reg1]) m_flags |= FLAG_ZERO;
            } else if (m_rtag[reg0] == TAG_FLOAT || m_rtag[reg1] == TAG_FLOAT) {
                double a = m_rtag[reg0] == TAG_FLOAT ? std::bit_cast<double>(m_regs[reg0]) : static_cast<double>(m_regs[reg0]);
                double b = m_rtag[reg1] == TAG_FLOAT ? std::bit_cast<double>(m_regs[reg1]) : static_cast<double>(m_regs[reg1]);
                if (a == b) m_flags |= FLAG_ZERO;
                if (a < b) m_flags |= FLAG_NEG;
            } else if (m_rtag[reg0] == 1 && m_rtag[reg1] == 1) {
				std::string s0 = readStringOperand(r0);
				std::string s1 = readStringOperand(r1);
				if (s0 == s1) m_flags |= FLAG_ZERO;
				if (s0 < s1)  m_flags |= FLAG_NEG;
			} else {
				int64_t val0 = m_regs[reg0];
				int64_t val1 = m_regs[reg1];
				if (val0 == val1) m_flags |= FLAG_ZERO;
				if (val0 < val1)  m_flags |= FLAG_NEG;
			}
			m_jitIp++;
			break;
		}

		case OpCode::Jmp: {
			auto &addr = instr.operands[0];
			size_t target;
			if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
				vm_panic("invalid jump target");
			}
			m_jitIp = target;
			break;
		}

		case OpCode::Jnz: {
			auto &addr = instr.operands[0];
			if (!(m_flags & FLAG_ZERO)) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
					vm_panic("invalid jnz target");
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::Jz: {
			auto &addr = instr.operands[0];
			if (m_flags & FLAG_ZERO) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
					vm_panic("invalid jz target");
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::Jn: {
			auto &addr = instr.operands[0];
			if (m_flags & FLAG_NEG) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
					vm_panic("invalid jn target");
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::Skip: {
			if (!instr.operands.empty()) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(instr.operands[0].intVal), target)) {
					vm_panic("invalid skip target");
				}
				m_jitIp = target;
			} else {
				halted = true;
			}
			break;
		}

		case OpCode::Call: {
			auto &addr = instr.operands[0];
			size_t target;
			if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
				vm_panic("invalid call target");
			}
			pushCallFrame(m_jitIp + 1, m_jitIp);
			m_jitIp = target;
			break;
		}

		case OpCode::NullChk: {
			auto &val = instr.operands[0];
			uint16_t reg = static_cast<uint16_t>(val.intVal);
			bool isNull = false;
			if (val.tag == OperandTag::Reg) {
				isNull = m_rtag[reg] == TAG_NULL;
			} else if (val.tag == OperandTag::Null) {
				isNull = true;
			}
			if (isNull) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(instr.operands[1].intVal), target)) {
					vm_panic("invalid null-check target");
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::CallInd: {
			auto &funcReg = instr.operands[0];
			uint16_t reg = static_cast<uint16_t>(funcReg.intVal);
			uint32_t nargs = static_cast<uint32_t>(instr.operands[1].intVal);
			uint32_t targetOffset = 0;
			if (m_rtag[reg] == TAG_HEAP) {
				uint32_t closureBase = static_cast<uint32_t>(m_regs[reg]);
				auto fnCell = m_heap.loadCell(closureBase, 0);
				if (!fnCell) vm_panic("closure has no readable function cell");
				targetOffset = static_cast<uint32_t>(fnCell->first);
				for (int i = static_cast<int>(nargs); i >= 1; --i) {
					uint8_t from = static_cast<uint8_t>(i);
					uint8_t to = static_cast<uint8_t>(i + 1);
					setReg(to, m_regs[from], m_rtag[from]);
				}
				setReg(1, m_regs[reg], m_rtag[reg]);
			} else {
				targetOffset = static_cast<uint32_t>(m_regs[reg]);
			}
			size_t target;
			if (!resolveJitTarget(targetOffset, target)) {
				vm_panic("invalid indirect call target");
			}
			pushCallFrame(m_jitIp + 1, m_jitIp);
			m_jitIp = target;
			break;
		}

		case OpCode::Ret: {
			while (!m_exceptionHandlers.empty() &&
			       m_exceptionHandlers.back().callDepth >= m_callStack.size()) {
				m_exceptionHandlers.pop_back();
			}
			if (m_callStack.empty()) {
				halted = true;
				return;
			}
			{
				const size_t returnIp = m_callStack.top().returnIp;
				restoreCallFrame();
				m_jitIp = returnIp;
			}
			break;
		}

		case OpCode::Write: {
				auto &val = instr.operands[0];
				if (val.tag == OperandTag::Imm64) {
					std::cout << val.intVal;
				} else if (val.tag == OperandTag::StrIdx) {
					if (val.strIdx < m_chunk.strings.size()) {
						std::cout << m_chunk.strings[val.strIdx];
					} else vm_panic("write received an invalid string index");
				} else if (val.tag == OperandTag::Reg) {
					uint16_t reg = static_cast<uint16_t>(val.intVal);
					if (m_rtag[reg] == 0) {
						std::cout << m_regs[reg];
					} else if (m_rtag[reg] == 1) {
						uint32_t strIdx = static_cast<uint32_t>(m_regs[reg]);
						if (strIdx < m_runtimeStrings.size()) {
							std::cout << m_runtimeStrings[strIdx];
						} else if (strIdx < m_chunk.strings.size()) {
							std::cout << m_chunk.strings[strIdx];
						} else vm_panic("write received an invalid string index");
					} else vm_panic("write received an invalid runtime tag " + std::to_string(m_rtag[reg]));
				} else vm_panic("write received an invalid operand tag");
				m_jitIp++;
				break;
			}

			case OpCode::Print: {
			auto &val = instr.operands[0];
			if (val.tag == OperandTag::Imm64) {
				std::cout << val.intVal << "\n";
			} else if (val.tag == OperandTag::StrIdx) {
				if (val.strIdx < m_chunk.strings.size()) {
					std::cout << m_chunk.strings[val.strIdx] << "\n";
				} else vm_panic("print received an invalid string index");
			} else if (val.tag == OperandTag::Reg) {
				uint16_t reg = static_cast<uint16_t>(val.intVal);
				if (m_rtag[reg] == 0) {
					std::cout << m_regs[reg] << "\n";
				} else if (m_rtag[reg] == 1) {
					uint32_t strIdx = static_cast<uint32_t>(m_regs[reg]);
					if (strIdx < m_runtimeStrings.size()) {
						std::cout << m_runtimeStrings[strIdx] << "\n";
					} else if (strIdx < m_chunk.strings.size()) {
						std::cout << m_chunk.strings[strIdx] << "\n";
					} else vm_panic("print received an invalid string index");
				} else vm_panic("print received an invalid runtime tag " + std::to_string(m_rtag[reg]));
			} else vm_panic("print received an invalid operand tag");
			m_jitIp++;
			break;
		}

		case OpCode::StrLen: {
			auto &src = instr.operands[0];
			auto &dstOp = instr.operands[1];
			uint16_t dstReg = static_cast<uint16_t>(dstOp.intVal);
			int64_t len = 0;
			if (src.tag == OperandTag::StrIdx) {
				if (src.strIdx < m_chunk.strings.size())
					len = static_cast<int64_t>(m_chunk.strings[src.strIdx].size());
			} else if (src.tag == OperandTag::Reg) {
				uint16_t srcReg = static_cast<uint16_t>(src.intVal);
				if (m_rtag[srcReg] == 1) {
					uint32_t strIdx = static_cast<uint32_t>(m_regs[srcReg]);
					if (strIdx < m_runtimeStrings.size())
						len = static_cast<int64_t>(m_runtimeStrings[strIdx].size());
					else if (strIdx < m_chunk.strings.size())
						len = static_cast<int64_t>(m_chunk.strings[strIdx].size());
				}
			}
			setReg(dstReg, len, TAG_INT);
			m_jitIp++;
			break;
		}
		case OpCode::ToStr: {
			auto &src = instr.operands[0];
			auto &dstOp = instr.operands[1];
			uint16_t dstReg = static_cast<uint16_t>(dstOp.intVal);
			std::string str;
			if (src.tag == OperandTag::StrIdx) {
				if (src.strIdx < m_chunk.strings.size())
					str = m_chunk.strings[src.strIdx];
			} else if (src.tag == OperandTag::Reg) {
				uint16_t srcReg = static_cast<uint16_t>(src.intVal);
				if (m_rtag[srcReg] == 0) {
					str = std::to_string(m_regs[srcReg]);
				} else if (m_rtag[srcReg] == 1) {
					uint32_t strIdx = static_cast<uint32_t>(m_regs[srcReg]);
					if (strIdx < m_runtimeStrings.size())
						str = m_runtimeStrings[strIdx];
					else if (strIdx < m_chunk.strings.size())
						str = m_chunk.strings[strIdx];
				}
			} else if (src.tag == OperandTag::Imm64) {
				str = std::to_string(src.intVal);
			}
			setReg(dstReg, static_cast<int64_t>(internRuntimeString(str)), TAG_STR);
			m_jitIp++;
			break;
		}


		case OpCode::VCall: {
			// Virtual call: obj_reg, slot_imm, nargs_imm, return_addr
			uint16_t objReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint32_t slot = static_cast<uint32_t>(instr.operands[1].intVal);
			int64_t heapOffset = m_regs[objReg];
			uint32_t typeId = m_heap.getTypeId(static_cast<uint32_t>(heapOffset));
			size_t target = 0;
			if (typeId < m_chunk.vtables.size() && slot < m_chunk.vtables[typeId].methodOffsets.size()) {
				if (resolveJitTarget(m_chunk.vtables[typeId].methodOffsets[slot], target)) {
                    size_t resume;
                    if (!resolveJitTarget(static_cast<uint32_t>(instr.operands[3].intVal), resume))
                        vm_panic("invalid virtual return address");
					pushCallFrame(resume, m_jitIp);
					m_jitIp = target;
					return;
				}
			}
            vm_panic("invalid virtual dispatch for type " + std::to_string(typeId) + " slot " + std::to_string(slot));
            return;
		}

		case OpCode::ICall: {
			// Interface call: obj_reg, iface_typeid_imm, method_idx_imm, nargs_imm, return_addr
			uint16_t objReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint32_t ifaceTypeId = static_cast<uint32_t>(instr.operands[1].intVal);
			uint32_t methodIdx = static_cast<uint32_t>(instr.operands[2].intVal);
			int64_t heapOffset = m_regs[objReg];
			uint32_t objTypeId = m_heap.getTypeId(static_cast<uint32_t>(heapOffset));
			if (objTypeId < m_chunk.vtables.size()) {
				const auto& vte = m_chunk.vtables[objTypeId];
				for (const auto& ite : vte.itable) {
					if (ite.interfaceTypeId == ifaceTypeId && methodIdx < ite.methodOffsets.size()) {
						size_t target;
						if (resolveJitTarget(ite.methodOffsets[methodIdx], target)) {
                            size_t resume;
                            if (!resolveJitTarget(static_cast<uint32_t>(instr.operands[4].intVal), resume))
                                vm_panic("invalid interface return address");
							pushCallFrame(resume, m_jitIp);
							m_jitIp = target;
							return;
						}
					}
				}
			}
            vm_panic("invalid interface dispatch for type " + std::to_string(objTypeId) + " interface " + std::to_string(ifaceTypeId));
            return;
		}

		case OpCode::TypeOf: {
			// Load typeId from object header into result register
			uint16_t objReg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint16_t resReg = static_cast<uint16_t>(instr.operands[1].intVal);
			int64_t heapOffset = m_regs[objReg];
			uint32_t typeId = m_heap.getTypeId(static_cast<uint32_t>(heapOffset));
			setReg(resReg, static_cast<int64_t>(typeId), TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Syscall: {
			uint16_t reg = static_cast<uint16_t>(instr.operands[0].intVal);
			uint8_t num = static_cast<uint8_t>(m_regs[reg]);
				invokeSyscall(num);
			m_jitIp++;
			break;
		}

		case OpCode::Halt:
			halted = true;
			break;

		default:
			vm_panic("unknown opcode " + std::to_string(static_cast<int>(instr.op)));
	}
}

// JitOperand and DecodedOperand carry the same three fields; this bridges them
// so trace output and --dump-bytecode render operands through one formatter.
static std::string formatTraceOperand(const VM::JitOperand &op, const Chunk &chunk) {
	return formatOperand(DecodedOperand{op.tag, op.intVal, op.strIdx}, chunk);
}

int VM::runJit(int maxExecuteCount) {
	int executeCount = 0;
	bool halted = false;
	m_jitIp = 0;
#ifndef NDEBUG
	// Debug-only invariant sweep: every register tagged TAG_HEAP must point at a
	// live slot.  This is what caught the acquire-before-release ordering bug.
	static const bool checkRoots = std::getenv("LOXIS_GC_CHECK") != nullptr;
	int reports = 0;
#endif

	while (m_jitIp < m_jitCode.size() && executeCount < maxExecuteCount && !halted) {
		executeCount++;
		const auto &instr = m_jitCode[m_jitIp];

#ifndef NDEBUG
		if (checkRoots && reports < 8) {
			for (int r = 0; r < 4096; r++) {
				if (m_rtag[r] != TAG_HEAP) continue;
				if (m_heap.valid(static_cast<uint32_t>(m_regs[r]))) continue;
				std::cerr << "[gc-check] step " << executeCount << " before "
				          << opcodeName(instr.op) << " @" << instr.byteOffset
				          << ": r" << r << " holds dead handle " << m_regs[r] << "\n";
				reports++;
				break;
			}
		}
#endif

		if (m_trace) {
			std::cerr << std::right << "[" << std::dec << std::setw(6) << std::setfill(' ') << executeCount << "] "
			          << "0x" << std::right << std::hex << std::setw(6) << std::setfill('0') << instr.byteOffset << std::dec << " " << std::setfill(' ')
			          << std::setw(7) << std::left << opcodeName(instr.op);
			for (size_t i = 0; i < instr.operands.size(); i++) {
				if (i > 0) std::cerr << ",";
				std::cerr << " " << formatTraceOperand(instr.operands[i], m_chunk);
			}
			std::cerr << std::endl;
		}

		try {
			execJitInstruction(instr, halted);
		} catch (const RuntimeUnwind&) {
			// raiseRuntime already unwound registers/calls and selected the landing pad.
		} catch (const HeapInvariantError& error) {
			panic(error.what());
		}
	}

	m_executed += static_cast<uint64_t>(executeCount);
	m_truncated = !halted && executeCount >= maxExecuteCount;
	return 0;
}

int VM::run() {
	constexpr int unlimited = 2147483647;
	return run(unlimited);
}

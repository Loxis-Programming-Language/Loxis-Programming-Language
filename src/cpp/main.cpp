#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include "Error.hpp"
#include "frontend/Lexer.hpp"
#include "frontend/Parser.hpp"
#include "ir/IRGen.hpp"
#include "backend/Compiler.hpp"
#include "vm/VM.hpp"

inline auto now() { return std::chrono::high_resolution_clock::now(); }

inline auto diff(auto t1, auto t2) {
	return std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
}

struct Argument {
	std::string source;
	int maxExecuteCount = 2147483647;  // unlimited by default
	bool postVerbose = false;
};

Argument parse(int argc, char *argv[]) {
	Argument em;
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <source_file> [max_instructions] [verbose]" << std::endl;
		throw std::runtime_error("missing source file.");
	}
	em.source = argv[1];
	if (argc >= 3) {
		std::string arg2 = argv[2];
		if (arg2 == "verbose") {
			em.postVerbose = true;
		} else {
			try {
				em.maxExecuteCount = std::stoi(arg2);
			} catch (const std::exception&) {
				throw std::runtime_error("invalid max_instructions: " + arg2);
			}
		}
	}
	if (argc >= 4) {
		em.postVerbose = true;
	}
	return em;
}

int main(int argc, char *argv[]) {
	auto em = parse(argc, argv);

	try {
		auto t0 = now();
		AST ast = Parser::parseFile(em.source);
		auto t1 = now();

		auto t2 = now();
		IRGen irgen;
		IRProgram program = irgen.generate(ast);
		auto t3 = now();

		auto t4 = now();
		Compiler compiler;
		Chunk chunk = compiler.compile(program);
		auto t5 = now();

		auto t6 = now();
		VM vm(chunk);
		vm.run(em.maxExecuteCount);
		auto t7 = now();

		if (em.postVerbose) {
			std::cerr << "Frontend:  " << diff(t0, t1) << " us" << std::endl;
			std::cerr << "IRGen:     " << diff(t2, t3) << " us" << std::endl;
			std::cerr << "Backend:   " << diff(t4, t5) << " us" << std::endl;
			std::cerr << "VM:        " << diff(t6, t7) << " us" << std::endl;
		}

		return 0;
	} catch (const GalVMError &e) {
		std::cerr << e.what() << std::endl;
		return 1;
	} catch (const std::exception &e) {
		std::cerr << "error: " << e.what() << std::endl;
		return 1;
	}
}

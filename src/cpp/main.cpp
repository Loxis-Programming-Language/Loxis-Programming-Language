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

#define time std::chrono::high_resolution_clock::now

auto diff(auto t1, auto t2) {
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
		em.maxExecuteCount = std::stoi(argv[2]);
	}
	if (argc >= 4) {
		em.postVerbose = true;
	}
	return em;
}

int main(int argc, char *argv[]) {
	auto em = parse(argc, argv);

	try {
		auto t0 = time();
		AST ast = Parser::parseFile(em.source);
		auto t1 = time();

		auto t2 = time();
		IRGen irgen;
		IRProgram program = irgen.generate(ast);
		auto t3 = time();

		auto t4 = time();
		Compiler compiler;
		Chunk chunk = compiler.compile(program);
		auto t5 = time();

		auto t6 = time();
		VM vm(chunk);
		vm.run(em.maxExecuteCount);
		auto t7 = time();

		if (em.postVerbose) {
			std::cerr << "Frontend:  " << diff(t0, t1) << " us" << std::endl;
			std::cerr << "IRGen:     " << diff(t2, t3) << " us" << std::endl;
			std::cerr << "Backend:   " << diff(t4, t5) << " us" << std::endl;
			std::cerr << "VM:        " << diff(t6, t7) << " us" << std::endl;
		}

		return 0;
	} catch (const RuntimeError &) {
		return 1;
	}
}

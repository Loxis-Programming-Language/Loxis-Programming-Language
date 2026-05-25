#include <iostream>
#include "frontend/Lexer.hpp"

int main() {
	Lexer lexer("fun greet(name) {\n    print(name)\n}\n\nfun main() {\n    greet(\"Loixs\")\n}\n", "test");
	auto tokens = lexer.tokenize();
	for (auto &t: tokens) {
		std::cout << "kind=" << (int) t.kind << " lexeme='" << t.lexeme << "'\n";
	}
}

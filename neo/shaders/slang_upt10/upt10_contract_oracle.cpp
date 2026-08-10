#include "upt10_contract_oracle.h"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) try {
	const rb::upt10::Corpus corpus = rb::upt10::MakeCorpus();
	const auto expected = rb::upt10::MakeExpected(corpus);
	rb::upt10::ValidateExpected(expected);
	std::cout << "UPT-30 CPU oracle passed: label-invariant, Jacobian-once, invalid/zero/extreme cases\n";
	if (argc == 2) {
		std::ofstream stamp(argv[1], std::ios::binary | std::ios::trunc);
		if (!stamp) throw std::runtime_error("failed to create oracle stamp");
		stamp << "UPT-30 CPU oracle passed\n";
	}
	return 0;
} catch (const std::exception& error) {
	std::cerr << "UPT-30 CPU oracle FAILED: " << error.what() << '\n';
	return 1;
}

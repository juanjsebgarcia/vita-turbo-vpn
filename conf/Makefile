# vtconf - .conf profile parser.
#
#   make test      build and run the host tests with sanitisers
#   make coverage  line and branch coverage of the library (needs llvm-cov)
#   make vita      cross-compile check in the pinned VitaSDK image (vita-check.sh)

CC      ?= cc
PROFDATA ?= xcrun llvm-profdata
LLVMCOV  ?= xcrun llvm-cov
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O1 -g
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
BUILD   := build

SRC     := vtconf.c vtconf_util.c
HDR     := vtconf.h ../include/vpnturbo.h
TEST    := tests/test_vtconf.c

.PHONY: test coverage vita clean

test: $(BUILD)/test_vtconf
	./$(BUILD)/test_vtconf

$(BUILD)/test_vtconf: $(SRC) $(HDR) $(TEST) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $(SRC) $(TEST)

coverage: | $(BUILD)
	$(CC) -std=c99 -O0 -g -fprofile-instr-generate -fcoverage-mapping \
		-o $(BUILD)/test_cov $(SRC) $(TEST)
	LLVM_PROFILE_FILE=$(BUILD)/cov.profraw ./$(BUILD)/test_cov
	$(PROFDATA) merge -o $(BUILD)/cov.profdata $(BUILD)/cov.profraw
	$(LLVMCOV) report $(BUILD)/test_cov -instr-profile=$(BUILD)/cov.profdata \
		-show-branch-summary $(SRC)

vita:
	./vita-check.sh

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

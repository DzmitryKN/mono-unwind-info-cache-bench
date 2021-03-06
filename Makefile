ORIGINAL:=_build/cachebench_original
OPTIMIZED:=_build/cachebench_optimized

MACHINE:=$(shell uname -m)
ifneq (,$(findstring armv,$(MACHINE)))
BENCHLOOPS:=30000
else
BENCHLOOPS:=300000
endif

bench: build
	@echo " *** BENCHMARKING ORIGINAL CODE ***"
	$(ORIGINAL) $(BENCHLOOPS)
	@echo " *** BENCHMARKING OPTIMIZED CODE ***"
	$(OPTIMIZED) $(BENCHLOOPS)

endurance_original: $(ORIGINAL)
	@echo " *** ENDURANCE TESTING ORIGINAL CODE UNTIL Ctrl+C ***"
	$(ORIGINAL)

endurance_optimized: $(OPTIMIZED)
	@echo " *** ENDURANCE TESTING OPTIMIZED CODE UNTIL Ctrl+C ***"
	$(OPTIMIZED)

build: $(ORIGINAL) $(OPTIMIZED)

$(ORIGINAL): cachebench.cpp _build
	g++ -std=c++11 -O2 -I. cachebench.cpp -o $(ORIGINAL)

$(OPTIMIZED): cachebench.cpp _build
	g++ -std=c++11 -O2 -DOPTIMIZED -I. cachebench.cpp -o $(OPTIMIZED)

_build:
	mkdir _build

clean:
	rm -f $(ORIGINAL) $(OPTIMIZED)
	rm -fd _build

.PHONY: clean

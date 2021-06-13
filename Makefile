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

endurance_original:
	@echo " *** ENDURANCE TESTING ORIGINAL CODE UNTIL Ctrl+C ***"
	$(ORIGINAL)

endurance_optimized:
	@echo " *** ENDURANCE TESTING OPTIMIZED CODE UNTIL Ctrl+C ***"
	$(ORIGINAL)

build: $(ORIGINAL) $(OPTIMIZED)

$(ORIGINAL): cachebench.cpp _build
	g++ -std=c++11 -O2 cachebench.cpp -o $(ORIGINAL)

$(OPTIMIZED): cachebench.cpp _build
	g++ -std=c++11 -O2 -DOPTIMIZED cachebench.cpp -o $(OPTIMIZED)

_build:
	mkdir _build

clean:
	rm -f $(ORIGINAL) $(OPTIMIZED)
	rm -d _build

.PHONY: clean

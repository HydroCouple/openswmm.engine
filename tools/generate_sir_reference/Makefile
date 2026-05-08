# Builds generate_sir_reference using genuine Flash-X RungeKutta CashKarp45 source.
# Requires gfortran (brew install gcc on macOS).
#
# Usage:
#   make              — build the generator
#   make run          — build and write reference.csv to the benchmark directory
#   make clean        — remove binary, object files, and .mod files

FLASHX_RK := /Users/corinnewiesner/Projects/Flash-X/source/numericalTools/RungeKutta

FC     := gfortran
FFLAGS := -O2 -fdefault-real-8 \
          -I. \
          -I$(FLASHX_RK)/RungeKuttaMain \
          -I/Users/corinnewiesner/Projects/Flash-X/source/Simulation \
          -B/usr/bin

TARGET := generate_sir_reference

BENCHMARK_CSV := ../../tests/benchmarks/manufactured/odesolve-sir-epidemic/reference.csv

# Flash-X sources in strict module-dependency order
FLASHX_SRCS := \
    $(FLASHX_RK)/RungeKuttaMain/RungeKutta_data.F90 \
    $(FLASHX_RK)/localAPI/rk_interface.F90 \
    $(FLASHX_RK)/RungeKutta_interface.F90 \
    $(FLASHX_RK)/RungeKuttaMain/rk_setButcherTableauRepository.F90 \
    $(FLASHX_RK)/RungeKuttaMain/rk_orderRKmethod.F90 \
    $(FLASHX_RK)/RungeKuttaMain/rk_stepEA.F90 \
    $(FLASHX_RK)/RungeKuttaMain/rk_stepEAC.F90 \
    $(FLASHX_RK)/RungeKuttaMain/RungeKutta_init.F90 \
    $(FLASHX_RK)/RungeKuttaMain/RungeKutta_step.F90 \
    $(FLASHX_RK)/RungeKuttaMain/RungeKutta_stepConfined.F90 \
    $(FLASHX_RK)/RungeKuttaMain/RungeKutta_stepSizeEstimate.F90 \
    $(FLASHX_RK)/RungeKuttaMain/RungeKutta_finalize.F90

LOCAL_SRCS := \
    Driver_interface.F90 \
    RuntimeParameters_interface.F90 \
    sir_ode.F90 \
    sir_driver.F90

ALL_SRCS := $(LOCAL_SRCS) $(FLASHX_SRCS)
ALL_OBJS := $(notdir $(ALL_SRCS:.F90=.o))

.PHONY: all run clean

all: $(TARGET)

# Compile each source to an object file sequentially (module files must
# exist before the files that use them are compiled).
%.o: %.F90
	$(FC) $(FFLAGS) -c -o $@ $<

# Symlink Flash-X files into the build directory so %.o rule finds them
# and .mod files land in the current directory.
$(TARGET): $(ALL_SRCS)
	@for src in $(ALL_SRCS); do \
	    base=$$(basename $$src); \
	    $(FC) $(FFLAGS) -c -o $${base%.F90}.o $$src || exit 1; \
	done
	$(FC) $(FFLAGS) -o $@ $(ALL_OBJS)

run: $(TARGET)
	./$(TARGET) > $(BENCHMARK_CSV)
	@echo "Written: $(BENCHMARK_CSV)"

clean:
	rm -f $(TARGET) *.mod *.o

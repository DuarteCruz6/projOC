TARGETS = cm2
PAPILIB_ALAMEDA=/run/current-system/sw/lib/libpapi.so
PAPILIB_TAGUS=/usr/lib/x86_64-linux-gnu/libpapi.so

all: $(TARGETS)

cm2: cm2.c
ifeq ($(shell test -e $(PAPILIB_ALAMEDA) && echo -n yes),yes)
	$(CC) $(CFLAGS) cm2.c $(PAPILIB_ALAMEDA) -o cm2
else
	$(CC) $(CFLAGS) cm2.c $(PAPILIB_TAGUS) -o cm2
endif

clean:
	rm -f $(TARGETS) *.o *.stderr *.stdout core *~
	rm *.out *.dat
	touch cm1_plot.dat

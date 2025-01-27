INCLUDES = -I/hilmar_install/ucx_install/include

LFLAGS= -Wl,-L/hilmar_install/ucx_install/lib,-rpath,/hilmar_install/ucx_install/lib,-lucp,-lucs,-lucs_signal,-luct,-lucm

FILES = test_ucp test_uct

all: $(FILES)

%.o: %.c
	gcc $(INCLUDES) -c $< -o $@

$(FILES): %: %.o
	gcc $(INCLUDES) $^ -o $@ $(LFLAGS)

clean:
	rm -f $(FILES) *.o


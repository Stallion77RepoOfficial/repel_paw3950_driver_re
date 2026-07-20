CC := clang
CFLAGS := -Wall -Wextra -Werror -O2 -I/opt/homebrew/include -I/opt/homebrew/include/libusb-1.0
LDFLAGS := -L/opt/homebrew/lib -lhidapi -lusb-1.0 -lncurses \
	-framework ApplicationServices

mousectl: mousectl.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

.PHONY: clean

clean:
	rm -f mousectl

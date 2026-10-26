PKGS := gtk4 gtk4-layer-shell-0 json-glib-1.0 libevdev hidapi-hidraw
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 $(shell pkg-config --cflags $(PKGS))
LIBS := $(shell pkg-config --libs $(PKGS)) -lm
SRCS := main.c config.c actions.c device.c draw.c theme.c haptics.c

.PHONY: all clean install

all: dialmenu

dialmenu: $(SRCS) dialmenu.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

clean:
	rm -f dialmenu

install: dialmenu
	install -d $(HOME)/.local/bin $(HOME)/.config/systemd/user
	install -m 755 dialmenu $(HOME)/.local/bin/dialmenu
	install -m 644 dialmenu.service $(HOME)/.config/systemd/user/dialmenu.service
	systemctl --user daemon-reload

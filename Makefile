FLAGS = -I$(STAGING_DIR)/usr/include/drm/ -ldrm
FLAGS += -Wall -O2 -D_FILE_OFFSET_BITS=64

all:
	$(CC) -o modeset-single-buffer modeset-single-buffer.c $(FLAGS)
	$(CC) -o modeset-double-buffer modeset-double-buffer.c $(FLAGS)
	$(CC) -o modeset-vsync modeset-vsync.c $(FLAGS)
	$(CC) -o modeset-atomic modeset-atomic.c $(FLAGS)

clean:
	rm -rf modeset-single-buffer modeset-double-buffer modeset-vsync modeset-atomic

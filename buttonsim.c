/*
 * buttonsim
 *
 * Create a simulated input device and generate some test input device events.
 * This can be used to verify the behaviour of the thingino-button daemon.
 *
 * Adapted from the example program provided in the Linux uinput documentation.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/uinput.h>

// List of key codes we want the device to be able to send
static const int key_codes[] = {
	KEY_ENTER,
	KEY_1,
	KEY_F12
};

static int fd;

static void emit(const int fd, const int type, const int code, const int val)
{
	struct input_event ie;

	ie.type = type;
	ie.code = code;
	ie.value = val;

	/*
	 * timestamp values below are ignored 
	 */

	// ie.time.tv_sec = 0;
	//ie.time.tv_usec = 0;

	int wrote = write(fd, &ie, sizeof(ie));
	if (wrote < 0) {
		perror("write");
		exit(EXIT_FAILURE);
	}
}

static void press(const int key_code)
{
	emit(fd, EV_KEY, key_code, 1);
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void release(const int key_code)
{
	emit(fd, EV_KEY, key_code, 0);
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void hold_down_for(const int t, const int key_code)
{
	press(key_code);
	sleep(t);
	release(key_code);
}

static void hold_down_for_ms(const int t, const int key_code)
{
	press(key_code);
	usleep(t * 1000);
	release(key_code);
}

/** Configure the uinput device
 *
 * Use the appropriate ioctl() calls to configure the uinput device to send
 * the key codes we want to test. If you want to send an additional key code,
 * make sure to add it to the list.
 */

static void configure_uinput_device(void)
{
	/*
	 * The ioctls below will enable the device that is about to be
	 * created, to pass key events, in this case the enter key.
	*/
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
		perror("ioctl(UI_SET_EVBIT)");
		exit(EXIT_FAILURE);
	}
		 
	int unsigned long i;
	for (i = 0; i < sizeof(key_codes) / sizeof(key_codes[0]); ++i) {
		if (ioctl(fd, UI_SET_KEYBIT, key_codes[i]) < 0) {
			perror("ioctl(UI_SET_KEYBIT)");
			exit(EXIT_FAILURE);
		}
	}

	// Configure a made up USB device for the testing
	struct uinput_setup usetup;
	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = 0x1234;
	usetup.id.product = 0x5678;
	strcpy(usetup.name, "Simulated buttons");

	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
		perror("ioctl(UI_DEV_SETUP)");
		exit(EXIT_FAILURE);
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("ioctl(UI_DEV_CREATE)");
			exit(EXIT_FAILURE);
	}

	/*
	 * On UI_DEV_CREATE the kernel will create the device node for this
	 * device. Insert a pause here so that userspace has time to detect
	 * and initialize the new device, and start listening to the event.
	 */
	sleep(1);
}

int main(void)
{
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("open");
		exit(EXIT_FAILURE);
	}

	configure_uinput_device();

	printf("You have five seconds to start thingino-button daemon\n");
	sleep(5);

	printf("Test 1: orphan key release: release event for ENTER key\n");
	release(KEY_ENTER);
	sleep(1);

	printf("Test 2: hold down ENTER key for half a second\n");
	hold_down_for_ms(500, KEY_ENTER);

	printf("Test 3: press 1 key whist ENTER key held down\n");
	press(KEY_ENTER);
	sleep(1);
	hold_down_for(1, KEY_1);
	sleep(1);
	release(KEY_ENTER);

	printf("Test 4: press an unknown KEY (F12)\n");
	hold_down_for_ms(500, KEY_F12);

	printf("Test 5: press ENTER for 20 seconds\n");
	hold_down_for(20, KEY_ENTER);


	/*
	* Give userspace some time to read the events before we destroy the
	* device with UI_DEV_DESTROY.
	*/
	sleep(2);

	ioctl(fd, UI_DEV_DESTROY);
	close(fd);

	return 0;
}

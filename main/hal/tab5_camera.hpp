// Service of CAMERA - sensor SC2356 (SC202CS) of Tab5, MIPI-CSI, 1600x1200.
//
// RESEARCHED, not invented: there was NO driver of
// camera in this project so far - just the CAM_RST (tab5_power_camera). The facts
// below came from BSP OFFICIAL of Espressif for this exact board
// (github.com/espressif/esp-bsp, bsp/m5stack_tab5), not of assumption:
//
// - Sensor: SC2356 / SC202CS, MIPI-CSI. Kconfig: CONFIG_CAMERA_SC202CS.
// - Pin of reset: NO direct GPIO - and the SAME CAM_RST as
// tab5_power_camera() already controls (expander 0, bit 6). the
// system of apps turned this on by itself via profile.camera=true; since that
// the camera turned to service (see the big warning below), who turns it on and
// Turns OFF the pin and tab5_camera_start()/stop() itself.
// - SCCB (the I2C of sensor, to configure it): G32/G31 - EXACTLY the
// same pins that M5Unified already uses for the INTERNAL bus
// (M5.In_I2C: touch, IMU, PMIC, expanders - see M5Unified.cpp,
// board_M5Tab5). In other words, the camera is just one more address in the SAME
// bus that the rest of the system already has and protects with the "only the
// task of interface speaks I2C".
//
// HOW DOES THIS NOT COLLIDE WITH WHAT ALREADY EXISTS: instead of opening one second
// bus on the same pins (prohibited - ESP-IDF does not leave two owners in
// same GPIO), tab5_camera_start() takes the handle that M5Unified already
// installed with i2c_master_get_bus_handle() - a function of IDF itself made
// exactly for "I want the bus that another component has already opened", without
// boot nothing new. Confirmed: M5GFX uses i2c_new_master_bus() (the
// NEW driver) by bottom of M5.In_I2C - hence the right handle and of type.
//
// THE REST (V4L2: open/ioctl/mmap in /dev/videoN) is the same default as
// official example of Espressif uses for THIS board
// (esp-bsp/examples/display_camera_video) - we just exchange LVGL for M5GFX and
// asynchronous callback by task_app_t (the template this project already uses to
// all background services, see kernel/app_task.h).
#pragma once

#include <stddef.h>

#include <stdbool.h>
#include <stdint.h>

// ------------------------------------------------------------------------
// IT HAS BECOME A SERVICE, NO LONGER A "CAMERA APP FEATURE"
// ("which internal sensors can become a service?" -> the camera was the only one
// piece of expensive hardware that only existed as a specific app
// was on screen).
// ------------------------------------------------------------------------
// REFERENCE COUNT, same template of tab5_sd.hpp: tab5_camera_start()
// can be called by MORE THAN ONE consumer at the same time (the Camera E app,
// for example, the command of console 'foto', or one future second app) - a
// first call turns on the true sensor, the following only count one more
// interested, and the sensor only turns off when the LAST call
// tab5_camera_stop(). Open the Camera app while the 'photo' command is still
// holds the camera (or vice versa) and holds: neither of them drops the other.
//
// ONE RESOLUTION AT A TIME, ON PURPOSE - there is only ONE pair of buffers of
// output (see TAB5_CAM_NBUF in tab5_camera.cpp). As long as there is already someone
// using the camera, one second tab5_camera_start() with width/height
// DIFFERENT and refused (returns false, log explains) instead of reconfigure
// the sensor at the bottom of who is already reading. Today there is only ONE consumer per
// resolution (Camera app uses the area of screen; 'photo' uses a fixed size
// itself) - the day this bothers you, the solution is a second pair of
// buffers, not this function.
//
// THE CAMERA POWERS ITSELF NOW - before, CAM_RST (the reset pin of
// sensor) was only on because the Camera app declared profile.camera=true, and
// KERNEL applied this before of ao_abrir() ran (see tab5_power.hpp). One
// service of truth cannot depend on ONE specific app being open
// to have power - that's why tab5_camera_start() now calls
// tab5_power_camera(true) ITSELF, in the same spirit as tab5_grove.cpp already
// connects the 5V rail alone for GPS/Mesh (see the warning there). The Camera app
// NO longer declares profile.camera - see the warning in app_camera.cpp.
//
// THE ONE DELICIOUS POINT: core.cpp still reapplies the energy profile
// (imu/mic/camera/etc) every time an app opens or closes, as a union of which
// ALL apps opened today declare - and WITHOUT KNOWING that this service exists,
// that reapplication would turn off CAM_RST in the storage of a session requested by
// outside of one app (the 'photo' command, for example) just because the app THAT OPENED
// AT THAT INSTANT don't ask for a camera. tab5_camera_active() (below) exists
// exactly to cover this hole:sistema_impor_preferencias() does OR with
// it, in the same way as it does with settings_grove() for Grove.
//
// INTERFACE TASK SO, DIRECT - find live with
// 'photo' command: these two functions touch SCCB (the internal I2C, the SAME
// bus of touch/IMU/energy/RTC), and the rule of this project is "only the
// task of interface speaks I2C" (no lock none in driver). app_camera.cpp
// can call directly because ao_open()/ao_close() JA run in this task.
// ANY other caller (one command of console, a future task of
// background) MUST go through System_Camera_Start()/System_Camera_Stop()
// (kernel/app.h) instead of calling these here - the bridge sends the request to the
// right task. Call these straight from another task and what produced "Set
// format regs fail" in first attempt of 'foto'.

// Turn on the sensor (or just add one more person interested, if it is already on - see
// the big warning above about reference count): I2C bus
// shared, esp_video_init, open the device, allocate the buffers of
// captures, records the PPA (scale + rotates 270 degrees - the sensor is
// mounted of side on board) and uploads the task that produces frames.
// 'width'/'height' and the size of buffer of OUTPUT (RGB565, already ready for
// pushImage) - the photo full of sensor (1600x1200) and reduced to this
// each frame.
//
// CALL SO FROM INTERFACE TASK - see large warning above. It doesn't depend
// more than profile.camera, it itself turns on CAM_RST. Returns false if any
// step fails (log explains which, including one request of resolution
// other than the one already in use) - in this case there is nothing to turn_off.
bool tab5_camera_start(int width, int height);

// Discards ONE interest (see count of reference above). So when
// LAST falls and the task goes down (task_app_terminar, with
// timeout - see the warning there about task abandoned of purpose instead of
// forcefully killed), the stream stops, the buffers disappear, the client of PPA exits and
// CAM_RST iteration remaining in reset. CALL SO FROM THE INTERFACE TASK (see the
// big warning above) - of where tab5_camera_start() was called
// (when_closing()). Safe to call even if tab5_camera_start() has never run,
// failed, or has already been stopped by another route.
void tab5_camera_stop(void);

// Are there any interested parties alive now (referral count > 0)? It exists for
// system_impor_preferencias() (kernel/core.cpp) does not cut CAM_RST for
// bottom of one consumer that is not the app in focus - see the big warning in the
// top of this file. Normal apps don't need to call this.
bool tab5_camera_active(void);

// WHERE THE PREVIEW IS WRITTEN. The camera task writes each
// frame, already rotated and scaled to width x height (os of
// tab5_camera_start), DIRECT in a rectangle of an RGB565 image - in practice
// a virtual screen of system (sistema_surface, kernel/app.h). No copy
// intermediary. 'buf' needs alignment of cache (the virtual screen has it).
// 'notify' (optional) is called task of camera every frame written -
//sistema_acordar_interface, for the interface to take the frame on time.
typedef struct {
  void*  buf;          // null = no target (only photos)
  size_t bytes;        // size total of buf
  int    pic_l, pic_a; // dimensions of entire image
  int    x, y;         // corner of rectangle inside it
  void (*notify)(void);
} tab5_cam_target_t;

void tab5_camera_set_target(const tab5_cam_target_t* d);   // nullptr = none

// true ONCE per frame written to the target since a latest call.
bool tab5_camera_frame_written(void);

// FULL RESOLUTION PHOTO: 1200x1600, the WHOLE frame of sensor rotated,
// unscaled - independent of size of preview.
//
// THE SENSOR RUN IN 1600x1200 MODE (sdkconfig.defaults). 1280x720 mode
// driver does not reduce the image: it turns on a WINDOW in the storage of sensor (columns
// 160-1447, lines 240-967), 80% of width and 60% of height - the photo comes out
// as if it had zoom.
//
// THE CAMERA IS ONLY CAPTURED; WHO RECORDS AND WHO ASKED FOR IT. The previous version recorded the
// .bmp in own task of camera, line by line: the preview stopped and the app
// was locked (START locked) for more than 10 s. Now the request and
// served in the next frame with ONE PPA operation (few ms) to the
// buffer of who requested; write and with the system worker (app) or with the
// task of console - see services/tab5_bmp.
#define TAB5_CAM_PHOTO_W 1200
#define TAB5_CAM_PHOTO_H 1600

// Right buffer for the photo (PSRAM, aligned to line of cache, the size that the
// PPA requires). Free with heap_caps_free.
uint16_t* tab5_camera_alloc_photo(void);
size_t    tab5_camera_photo_bytes(void);

// Request the next photo for 'target' (of tab5_camera_alloc_photo). false = a
// camera is not enabled or there is already one request pending. 'target' is written by
// task of camera until tab5_camera_photo_ready() return_card true - do not release
// before that (tab5_camera_stop() also waits for the task to finish).
bool tab5_camera_request_photo(uint16_t* target);

// true ONCE when the requested photo arrived in buffer (ok = PPA was successful).
bool tab5_camera_photo_ready(bool* ok);

// SENSOR REGISTER, for diagnostics ('camera' of console command).
// value >= 0 writes before of read; -1 just read. false = camera off or
// SCCB failed. What is written is valid until the camera turn_off: on turn_on of new
// the driver reapplies the initialization table.
bool tab5_camera_reg(uint16_t reg, int value, uint8_t* value_read);

# THERMAL EDGE

Android thermal-vision overlay app for InfiRay / Xinfrared T2 Pro.

THERMAL EDGE reads the T2 Pro as a USB UVC camera, extracts thermal luminance edges from its 256×196 YUYV stream, and overlays those edges on the phone's rear camera in real time.

## v0.1.0 goals

- Rear camera preview with CameraX
- USB permission and T2 Pro / generic UVC detection
- Native libusb + libuvc streaming
- 256×192 thermal luminance extraction (bottom metadata rows ignored)
- Real-time Sobel-style edge extraction
- Thermal-edge overlay over the phone camera
- Manual alignment controls: X/Y offset, scale, rotation, opacity, threshold
- Dark HUD-style UI and live status panel
- GitHub Actions debug APK build

## Notes

T2 Pro devices have been observed exposing a UVC YUYV stream around 256×196 at 25 fps. The first implementation deliberately uses the visible thermal luminance for edge extraction, so it does not depend on proprietary temperature-decoding SDKs.

Temperature readout and distance-profile calibration can be added later without changing the overlay pipeline.

# Code walkthrough

A guided read of the application, in the order a frame travels. For each file:
what it is for, the lines that matter, and the questions a reviewer is likely
to ask. Paths are relative to `app/`.

## Reading order

1. `src/main.cpp` (40 lines of logic)
2. `src/channel.cpp`, `run()` is the frame loop
3. `src/camera_source.cpp`
4. `src/sensor_model.cpp`
5. `src/soft_isp.cpp`
6. `src/isp_control.cpp`
7. `src/encoder.cpp`
8. the small ones: `thread_pool`, `snapshot`, `control_socket`, `frame_stats`, `config`, `log`

## `src/main.cpp`

Parses the configuration, blocks SIGINT, SIGTERM and SIGUSR1, creates the
`Channel`, starts one thread that waits for those signals, and runs the
channel.

- **Why block the signals before anything else?** A new thread inherits the
  signal mask of its creator. Blocking first means every thread created later
  (the ISP pool, libcamera's, GStreamer's) has them blocked, so the kernel can
  deliver them only to the thread sitting in `sigwait()`.
- **Why `sigwait()` and not a handler?** A signal handler may only call
  async-signal-safe functions: no mutexes, no `malloc`, no logging. `sigwait()`
  returns the signal as an ordinary value in an ordinary thread, where anything
  is allowed.
- **Why `pthread_kill(..., SIGTERM)` at the end?** If the channel stops by
  itself (`--duration`, or an error), the signal thread is still blocked in
  `sigwait()`. Sending it SIGTERM wakes it so it can be joined.

## `src/channel.cpp`

Owns all the parts and runs the loop.

`init()`: create the output directory, open the camera, log what the ISP input
will be, start the encoder, start the control socket.

`run()`, once per frame:

1. `camera_.waitFrame()`: returns a completed request, or times out after
   200 ms so the loop can still report statistics and notice a stop request.
2. `applyControls()`: picks up changes made through the socket.
3. RAW input: the camera's RAW buffer if it has one, otherwise
   `sensor_.capture()` (the mosaic step). The exposure and gain used are the
   ones the AE computed on the *previous* frame, as on a real sensor.
4. `SoftIsp::measure()` then `controller_.update()`: statistics, AWB, AE.
5. `encoder_.acquire()`: get an encoder buffer to write into.
6. `isp_.process()`: all ISP stages, output straight into that buffer.
7. `encoder_.submit()`: hand over. Blocks if the encoder is behind.
8. `camera_.recycle()`: the request goes back to the free list.
9. `stats_.addFrame()`.

`applyControls()` and `sendToCamera()`: for each control, ask the camera
whether it supports it (`ControlInfoMap` lookup). If yes, put it in the
`ControlList` of the next request. Either way the software ISP gets the full
set, because it works on RAW data that no camera-side processing has touched.

`handleCommand()` / `setControl()`: the socket protocol. They run on the
socket thread, so they only touch `desired_` under `controlMutex_` and set a
dirty flag. The frame loop applies the change at the start of the next frame.

`saveSnapshot()`: computes the PSNR against the source picture (only possible
with the mosaic step, where the source is known) and queues the PNG.

- **Why recycle the request at the end and not right after the mosaic step?**
  Simplicity; the DMABUF is no longer needed after step 3, so it could be
  earlier. On a real camera, returning the buffer earlier lowers the chance of
  starving the sensor.
- **What if the encoder dies?** `submit()` returns false or `healthy()` sees
  an error on the bus; the loop ends with exit code 1 and systemd restarts the
  service (`Restart=on-failure`).

## `src/camera_source.cpp`

The only file that includes libcamera.

`open()`:
- `CameraManager::start()`, list `cameras()`, choose one.
- `Camera::acquire()`: exclusive access.
- `logControls()`: iterate `camera->controls()` (the `ControlInfoMap`).
- `configure()`.
- `FrameBufferAllocator::allocate()` per stream, then `mapBuffers()`.
- Create one `Request` per buffer with `createRequest()` and `addBuffer()`.
- Connect `requestCompleted` to `onRequestCompleted()`.

`configure()`:
- `generateConfiguration({Viewfinder, Raw})`. If the pipeline handler cannot do
  RAW it returns nothing; then `generateConfiguration({Viewfinder})`.
- Set the wanted format and size, call `validate()`. libcamera may *adjust*
  the request (here YUV420 becomes NV12); the code accepts NV12 or YUV420.
- `Camera::configure()`.

`mapBuffers()`: each `FrameBuffer::Plane` has a DMABUF fd, an offset and a
length. Several planes can share one fd, so each fd is mapped once over its
full extent and plane pointers are `base + offset`.

`waitFrame()` and `queueDueRequests()`: the request pump.
- `free_` holds requests that can be queued. `completed_` holds requests
  libcamera has finished.
- With pacing on, one request is queued per frame period (`nextDue_`). If the
  loop fell behind, the schedule restarts from now instead of bursting.
- With pacing off (real sensor), every free request is queued immediately.
- The wait uses a condition variable with a deadline: the next frame tick or
  the caller's timeout, whichever is first.

`onRequestCompleted()`: runs on libcamera's thread. It must not block that
thread, so it pushes the pointer to `completed_` and notifies.

`describe()`: checks `Request::status()` and the buffer's
`FrameMetadata::status`, detects sequence gaps (drops), and fills a
`CapturedFrame` with pointers into the mapped DMABUF. Nothing is copied.

`recycle()`: `Request::reuse(ReuseBuffers)` clears the request's controls and
metadata but keeps its buffers attached, so it can be queued again.

- **What is request recycling and why does it matter?** Creating a request
  allocates. Doing that per frame fragments the heap and costs time. A fixed
  set, reused, gives constant memory and no per-frame allocation.
- **Why is the destructor ordered the way it is?** Requests point at buffers,
  buffers belong to the allocator, the allocator needs the camera. Tear down
  in reverse: stop, unmap, requests, allocator, release camera, stop manager.
- **Where would a dropped frame show up?** `FrameMetadata::sequence` jumps by
  more than one. `dropped_` counts the gap.

## `src/sensor_model.cpp`

The mosaic step. See REPORT section 4 for the seven steps.

- `YuvTables`: integer YUV to RGB, Q16, as five 256-entry tables.
- `lin[]`: sRGB code to linear light, Q15.
- `k[c][j]`: for output colour `c`, the row of the inverse CCM, times the
  illuminant factor, times exposure, times the code range. One dot product
  per pixel produces the RAW value for that pixel's filter colour.
- `(acc + (1 << 22)) >> 23`: Q15 times Q8 is Q23; adding half before shifting
  rounds to nearest.
- The CFA: `isRedRow` and the column parity decide whether the site is R, G
  or B. `redRow()` / `redCol()` in `image.h` encode the four Bayer orders.
- `referenceRgb()`: the same YUV to RGB, used as the ground truth for PSNR.

- **Why invert the CCM here?** So that the ISP's CCM has something real to
  correct. If the sensor produced sRGB directly, the CCM stage would be
  untestable.
- **Why linear light?** White balance and colour matrices are only correct on
  values proportional to light. Gamma comes last in an ISP for the same reason.

## `src/soft_isp.cpp`

`stage::buildBlcAwbLut()`: stages 1 and 2 as one table per colour:
`(code - black) * gain`, scaled so sensor white becomes 4095, clipped.

`stage::demosaicRow()`: stage 3. Works on three padded lines (above, current,
below). At a red or blue site: green is the mean of the 4 edge neighbours, the
opposite colour is the mean of the 4 diagonals. At a green site: one colour
is left/right, the other above/below; which is which depends on the row.

`stage::ccmRow()`: stage 4. `(m * rgb + 512) >> 10` is a rounded Q10 multiply.
Results are clamped to 0..4095.

`stage::buildGammaLut()` / `gammaRow()`: stage 5. sRGB curve, then
`(v - 0.5) * contrast + 0.5 + brightness`.

`stage::lumaRow()` / `chromaRows()`: stage 6. BT.709 limited range; chroma is
computed from the sum of each 2x2 block.

`SoftIsp::measure()`: AWB/AE statistics from one quad per 8x8 block.

`SoftIsp::process()`:
- Builds the tables.
- `correctLine()`: applies stages 1+2 to one RAW row into a padded line.
  Border rows and columns are mirrored by two samples so the Bayer phase is
  preserved.
- Splits the height into `pool_.size()` bands on even row boundaries.
- Per band, keeps a sliding window of three corrected lines and runs stages 3
  to 6 on one row at a time. Rows stay in cache between stages.
- Chroma is written on odd rows, when both rows of the pair are available.

- **Why bilinear?** It is the reference algorithm the assignment names. It is
  cheap and has known artefacts (soft edges, colour fringes).
- **Why tables?** A table lookup replaces a subtract, multiply, shift and two
  clamps, and makes gamma (a `pow`) free per pixel.
- **Is the threaded result identical to the single-threaded one?** Yes, bit
  for bit. Each band reads its neighbours' RAW rows but writes only its own
  output rows. `testThreadInvariance` checks it.
- **Why did the loop index have to be signed?** `cur[x - 1]` at `x == 0` must
  address the padding sample at index -1. With an unsigned `x` it wraps.

## `src/isp_control.cpp`

`IspControls` mirrors the libcamera controls. `IspController::update()`:

- White balance: manual gains, or a preset table, or gray-world with a
  low-pass filter (`alpha = 0.25`) after the first estimate.
- Exposure: manual, or the AE loop. `total = exposure * gain * sqrt(ratio)`
  moves half-way (in stops) to the target each frame; the result is split into
  exposure time first, then gain.

- **Why a dead band?** Without it the loop would keep making tiny corrections
  around the target and the picture would shimmer.
- **Why exposure before gain?** Longer exposure collects more light; gain
  amplifies noise along with signal.

## `src/encoder.cpp`

`start()`: `gst_parse_launch()` on the configured string, find `appsrc`, set
caps (I420, size, frame rate, BT.709), set `block=true` and `max-bytes` to two
frames, go to PLAYING.

`acquire()`: allocate a `GstBuffer`, map it for writing, expose the three
planes as an `I420Frame`. `submit()`: unmap, set the timestamp, push. The push
transfers ownership, so there is no unref.

`stop()`: send end-of-stream and wait for the EOS message. Without that the
MP4 muxer never writes its index and the file is unplayable.

- **Where is the back-pressure?** `block=true` with `max-bytes`. A third frame
  cannot be pushed until x264 has taken one.
- **Timestamps?** The sensor timestamp from libcamera, relative to the first
  frame. The video plays at the rate frames were really captured.

## The small files

- `thread_pool.cpp`: `parallelFor(count, fn)`. Workers and the caller take
  indices from a shared counter under a mutex; the caller waits until
  `pending_` reaches zero.
- `snapshot.cpp`: a queue and one thread. PNG is written to `name.part` and
  renamed, so a reader never sees half a file. `setjmp` is how libpng reports
  errors.
- `control_socket.cpp`: `poll()` on the listening socket and an `eventfd`; the
  eventfd is how `stop()` wakes the thread. One command per connection.
- `frame_stats.cpp`: process CPU time from `CLOCK_PROCESS_CPUTIME_ID`, RSS from
  `/proc/self/statm`.
- `config.cpp`: one `setOption(key, value)` used by both the file parser and
  the command line.
- `log.cpp`: the `<6>` prefix is the syslog priority; journald reads it.

## The Yocto recipes in one paragraph each

- `isp-channel_1.0.bb`: `inherit cmake pkgconfig systemd useradd`. Sources come
  from `files/isp-channel`. `do_install:append` adds the unit, the config and
  the udev rule. `USERADD_PARAM` creates the service user in group `video`.
  `RDEPENDS` lists the GStreamer plugin packages that hold the elements named
  in the pipeline string; a missing one would only fail at run time.
- `virtual-camera-config_1.0.bb`: installs `virtual.yaml` where libcamera
  looks for it, and the frames. `allarch` because it is only data.
- `libyuv_git.bb`: CMake build, custom `do_install` that keeps the static
  library and headers and writes `libyuv.pc`.
- `libcamera_%.bbappend`: adds the four patches and, for QEMU machines, the
  libyuv and jpeg build dependencies.
- `linux-yocto_%.bbappend`: adds the `.cfg` fragment; the kernel recipe merges
  fragments from `SRC_URI` automatically.
- `isp-image.bb`: `require core-image-minimal.bb`, then image features, extra
  packages, extra space, and the QEMU CPU and memory settings.
- `conf/distro/isp.conf`: see README section 4.

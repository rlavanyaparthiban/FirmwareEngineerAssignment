FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# libcamera's virtual pipeline handler allocates its frame buffers as DMABUFs
# from a DMA heap or from udmabuf. Neither is enabled in the stock qemuarm64
# kernel configuration.
SRC_URI:append:qemuall = " file://virtual-camera-dmabuf.cfg"

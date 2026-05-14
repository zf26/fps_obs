/*
 * DXGI-based screen capture using Output Duplication API
 */

#define COBJMACROS
#include "screen-capture.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <assert.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")

struct screen_capture {
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	IDXGIAdapter *matched_adapter; // adapter matched to the target output
	IDXGIOutputDuplication *duplication;
	ID3D11Texture2D *staging_texture;

	int monitor_idx;
	int width;
	int height;

	struct capture_frame frame;
	uint8_t *pixel_buffer;
	size_t pixel_buffer_size;

	bool access_lost; // set on DXGI_ERROR_ACCESS_LOST, cleared on successful capture
};

static HRESULT create_d3d11_device(ID3D11Device **device, ID3D11DeviceContext **context)
{
	*device = NULL;
	*context = NULL;

	UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
	HRESULT hr = D3D11CreateDevice(
		NULL, D3D_DRIVER_TYPE_HARDWARE,
		NULL, flags,
		NULL, 0, D3D11_SDK_VERSION,
		device, NULL, context);

	if (SUCCEEDED(hr)) {
		obs_log(LOG_INFO, "D3D11 device created on hardware adapter");
		return hr;
	}

	obs_log(LOG_WARNING, "Hardware D3D11 device failed (0x%08lX), trying WARP",
		(unsigned long)hr);
	hr = D3D11CreateDevice(
		NULL, D3D_DRIVER_TYPE_WARP,
		NULL, 0,
		NULL, 0, D3D11_SDK_VERSION,
		device, NULL, context);

	if (SUCCEEDED(hr)) {
		obs_log(LOG_INFO, "D3D11 device created on WARP (software)");
		return hr;
	}

	obs_log(LOG_ERROR, "D3D11CreateDevice failed: 0x%08lX", (unsigned long)hr);
	return hr;
}

static HRESULT create_device_on_adapter(IDXGIAdapter *adapter, ID3D11Device **device,
	ID3D11DeviceContext **context)
{
	*device = NULL;
	*context = NULL;

	UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
	HRESULT hr = D3D11CreateDevice(
		adapter, D3D_DRIVER_TYPE_UNKNOWN,
		NULL, flags,
		NULL, 0, D3D11_SDK_VERSION,
		device, NULL, context);

	if (SUCCEEDED(hr)) return hr;

	hr = D3D11CreateDevice(
		adapter, D3D_DRIVER_TYPE_UNKNOWN,
		NULL, 0,
		NULL, 0, D3D11_SDK_VERSION,
		device, NULL, context);

	return hr;
}

static void log_adapter_info(IDXGIAdapter *adapter)
{
	DXGI_ADAPTER_DESC desc;
	if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
		char name[128];
		int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
			name, (int)sizeof(name) - 1, NULL, NULL);
		if (len > 0) name[len] = '\0';
		else name[0] = '\0';
		obs_log(LOG_INFO, "  Adapter: %s (LUID: %08x:%08x)",
			name,
			desc.AdapterLuid.HighPart,
			desc.AdapterLuid.LowPart);
	}
}

static HRESULT find_working_adapter_for_output(IDXGIOutput *target_output,
	ID3D11Device **out_device, ID3D11DeviceContext **out_context, IDXGIAdapter **out_adapter)
{
	*out_device = NULL;
	*out_context = NULL;
	*out_adapter = NULL;

	IDXGIFactory *factory = NULL;
	IDXGIAdapter *adapter = NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *ctx = NULL;

	// Create factory directly — target_output is only used to hint which monitor,
	// but enumerating all adapters covers all outputs on all GPUs anyway.
	// We also need dxgi.dll loaded, which happens via D3D11CreateDevice.
	typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY)(REFIID, void **);
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	if (!dxgi) {
		obs_log(LOG_ERROR, "Failed to load dxgi.dll");
		return E_FAIL;
	}
	PFN_CREATE_DXGI_FACTORY pfnCreate = (PFN_CREATE_DXGI_FACTORY)
		(void *)GetProcAddress(dxgi, "CreateDXGIFactory");
	if (!pfnCreate) {
		obs_log(LOG_ERROR, "CreateDXGIFactory not found in dxgi.dll");
		FreeLibrary(dxgi);
		return E_FAIL;
	}

	// Try to create factory with VIDEO_SUPPORT feature support (IDXGIFactory2)
	// IDXGIFactory2 supports IsCurrent and duplicate API, fall back to factory1
	HRESULT hr = pfnCreate(&IID_IDXGIFactory2, (void **)&factory);
	if (FAILED(hr)) {
		hr = pfnCreate(&IID_IDXGIFactory, (void **)&factory);
	}
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "CreateDXGIFactory failed: 0x%08lX", (unsigned long)hr);
		FreeLibrary(dxgi);
		return hr;
	}

	// Enumerate adapters and try DuplicateOutput on each
	for (UINT i = 0; ; i++) {
		if (adapter) { IDXGIAdapter_Release(adapter); adapter = NULL; }
		hr = IDXGIFactory_EnumAdapters(factory, i, &adapter);
		if (hr == DXGI_ERROR_NOT_FOUND) break;
		if (FAILED(hr)) continue;

		log_adapter_info(adapter);

		hr = create_device_on_adapter(adapter, &device, &ctx);
		if (FAILED(hr)) continue;

		// Try DuplicateOutput with this device on each output of this adapter
		for (UINT j = 0; ; j++) {
			IDXGIOutput *enum_output = NULL;
			hr = IDXGIAdapter_EnumOutputs(adapter, j, &enum_output);
			if (hr == DXGI_ERROR_NOT_FOUND) break;
			if (FAILED(hr)) continue;

			IDXGIOutput1 *output1 = NULL;
			hr = IDXGIOutput_QueryInterface(enum_output, &IID_IDXGIOutput1, (void **)&output1);
			if (SUCCEEDED(hr)) {
				IDXGIOutputDuplication *dup = NULL;
				hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)device, &dup);
				IDXGIOutputDuplication_Release(dup);
				IDXGIOutput1_Release(output1);

				if (SUCCEEDED(hr)) {
					// Found a working adapter + output combination
					IDXGIOutput_Release(enum_output);
					*out_device = device;
					*out_context = ctx;
					*out_adapter = adapter;
					IDXGIFactory_Release(factory);
					FreeLibrary(dxgi);
					obs_log(LOG_INFO, "Found working adapter for DuplicateOutput (adapter %d, output %d)", i, j);
					return S_OK;
				}
			}
			IDXGIOutput_Release(enum_output);
		}

		ID3D11DeviceContext_Release(ctx);
		ID3D11Device_Release(device);
		device = NULL;
		ctx = NULL;
	}

	if (adapter) IDXGIAdapter_Release(adapter);
	IDXGIFactory_Release(factory);
	FreeLibrary(dxgi);
	return E_FAIL;
}

struct screen_capture *screen_capture_create(int monitor_idx)
{
	struct screen_capture *sc = calloc(1, sizeof(struct screen_capture));
	if (!sc) return NULL;

	sc->monitor_idx = monitor_idx;

	// Step 1: create initial device
	HRESULT hr = create_d3d11_device(&sc->device, &sc->context);
	if (FAILED(hr)) {
		free(sc);
		return NULL;
	}

	// Step 2: get the target output
	IDXGIOutput *target_output = NULL;
	DXGI_OUTPUT_DESC output_desc;
	{
		IDXGIDevice *dxgi_device = NULL;
		IDXGIAdapter *adapter = NULL;
		hr = ID3D11Device_QueryInterface(sc->device, &IID_IDXGIDevice, (void **)&dxgi_device);
		if (SUCCEEDED(hr)) {
			hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
			if (SUCCEEDED(hr)) {
				hr = IDXGIAdapter_EnumOutputs(adapter, (UINT)monitor_idx, &target_output);
				if (SUCCEEDED(hr)) {
					IDXGIOutput_GetDesc(target_output, &output_desc);
				}
				IDXGIAdapter_Release(adapter);
			}
			IDXGIDevice_Release(dxgi_device);
		}
		if (!target_output) {
			obs_log(LOG_ERROR, "Failed to enumerate monitor %d", monitor_idx);
			screen_capture_destroy(sc);
			return NULL;
		}
	}

	sc->width  = (int)(output_desc.DesktopCoordinates.right  - output_desc.DesktopCoordinates.left);
	sc->height = (int)(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);

	// Step 3: try DuplicateOutput with the initial device
	IDXGIOutput1 *output1 = NULL;
	hr = IDXGIOutput_QueryInterface(target_output, &IID_IDXGIOutput1, (void **)&output1);
	if (SUCCEEDED(hr)) {
		hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)sc->device, &sc->duplication);
		IDXGIOutput1_Release(output1);
	}
	IDXGIOutput_Release(target_output);

	// Step 4: on Optimus laptops the first device may not be the right one — try all adapters
	if (FAILED(hr) || !sc->duplication) {
		obs_log(LOG_WARNING, "DuplicateOutput with default device failed (0x%08lX), "
			"enumerating all adapters for Optimus compatibility...",
			(unsigned long)hr);

		// Release the failed device so find_working_adapter_for_output can try its own
		ID3D11DeviceContext_Release(sc->context);
		ID3D11Device_Release(sc->device);
		sc->device = NULL;
		sc->context = NULL;

		hr = find_working_adapter_for_output(NULL, &sc->device, &sc->context, &sc->matched_adapter);
		if (FAILED(hr)) {
			obs_log(LOG_ERROR, "No adapter supported DuplicateOutput for monitor %d", monitor_idx);
			free(sc);
			return NULL;
		}

		// Re-enumerate the target output on the new device
		IDXGIOutput *retry_output = NULL;
		IDXGIDevice *dxgi_device = NULL;
		IDXGIAdapter *enum_adapter = NULL;
		hr = ID3D11Device_QueryInterface(sc->device, &IID_IDXGIDevice, (void **)&dxgi_device);
		if (SUCCEEDED(hr)) {
			hr = IDXGIDevice_GetAdapter(dxgi_device, &enum_adapter);
			if (SUCCEEDED(hr)) {
				hr = IDXGIAdapter_EnumOutputs(enum_adapter, (UINT)monitor_idx, &retry_output);
				if (SUCCEEDED(hr)) {
					IDXGIOutput1 *out1 = NULL;
					hr = IDXGIOutput_QueryInterface(retry_output, &IID_IDXGIOutput1,
								       (void **)&out1);
					if (SUCCEEDED(hr)) {
						hr = IDXGIOutput1_DuplicateOutput(out1, (IUnknown *)sc->device,
									       &sc->duplication);
						IDXGIOutput1_Release(out1);
					}
					IDXGIOutput_Release(retry_output);
				}
				IDXGIAdapter_Release(enum_adapter);
			}
			IDXGIDevice_Release(dxgi_device);
		}

		if (FAILED(hr) || !sc->duplication) {
			obs_log(LOG_ERROR, "DuplicateOutput still failed after adapter enumeration");
			screen_capture_destroy(sc);
			return NULL;
		}
	}

	obs_log(LOG_INFO, "Screen capture created for monitor %d (%dx%d)", monitor_idx, sc->width, sc->height);
	return sc;
}

void screen_capture_destroy(struct screen_capture *sc)
{
	if (!sc) return;

	if (sc->duplication) {
		IDXGIOutputDuplication_ReleaseFrame(sc->duplication);
		IDXGIOutputDuplication_Release(sc->duplication);
	}
	if (sc->staging_texture) ID3D11Texture2D_Release(sc->staging_texture);
	if (sc->context) ID3D11DeviceContext_Release(sc->context);
	if (sc->device) ID3D11Device_Release(sc->device);
	if (sc->matched_adapter) IDXGIAdapter_Release(sc->matched_adapter);

	free(sc->pixel_buffer);
	free(sc);
}

bool screen_capture_capture(struct screen_capture *sc)
{
	if (!sc || !sc->duplication) return false;

	IDXGIResource *resource = NULL;
	DXGI_OUTDUPL_FRAME_INFO frame_info;

	HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(sc->duplication, 0, &frame_info, &resource);

	// Clear access_lost flag; it will be set again if ACCESS_LOST occurs below
	sc->access_lost = false;

	if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
	if (hr == DXGI_ERROR_ACCESS_LOST) {
		obs_log(LOG_WARNING, "DXGI access lost, need to recreate");
		sc->access_lost = true;
		return false;
	}
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "AcquireNextFrame failed: 0x%08lX", (unsigned long)hr);
		return false;
	}

	ID3D11Texture2D *texture = NULL;
	hr = IDXGIResource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&texture);
	IDXGIResource_Release(resource);
	if (FAILED(hr) || !texture) {
		IDXGIOutputDuplication_ReleaseFrame(sc->duplication);
		return false;
	}

	D3D11_TEXTURE2D_DESC tex_desc;
	ID3D11Texture2D_GetDesc(texture, &tex_desc);

	if (!sc->staging_texture || sc->width != (int)tex_desc.Width || sc->height != (int)tex_desc.Height) {
		if (sc->staging_texture) ID3D11Texture2D_Release(sc->staging_texture);

		sc->width = (int)tex_desc.Width;
		sc->height = (int)tex_desc.Height;

		D3D11_TEXTURE2D_DESC staging_desc;
		ZeroMemory(&staging_desc, sizeof(staging_desc));
		staging_desc.Width = tex_desc.Width;
		staging_desc.Height = tex_desc.Height;
		staging_desc.MipLevels = 1;
		staging_desc.ArraySize = 1;
		staging_desc.Format = tex_desc.Format;
		staging_desc.SampleDesc.Count = 1;
		staging_desc.Usage = D3D11_USAGE_STAGING;
		staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		hr = ID3D11Device_CreateTexture2D(sc->device, &staging_desc, NULL, &sc->staging_texture);
		if (FAILED(hr)) {
			obs_log(LOG_ERROR, "Failed to create staging texture: 0x%08lX", (unsigned long)hr);
			ID3D11Texture2D_Release(texture);
			IDXGIOutputDuplication_ReleaseFrame(sc->duplication);
			return false;
		}
	}

	ID3D11DeviceContext_CopyResource(sc->context, (ID3D11Resource *)sc->staging_texture, (ID3D11Resource *)texture);
	ID3D11Texture2D_Release(texture);
	IDXGIOutputDuplication_ReleaseFrame(sc->duplication);

	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = ID3D11DeviceContext_Map(sc->context, (ID3D11Resource *)sc->staging_texture, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) return false;

	int stride = (int)mapped.RowPitch;
	int row_size = sc->width * 4;
	size_t needed = (size_t)stride * sc->height;

	if (!sc->pixel_buffer || sc->pixel_buffer_size < needed) {
		free(sc->pixel_buffer);
		sc->pixel_buffer = malloc(needed);
		sc->pixel_buffer_size = needed;
	}

	if (sc->pixel_buffer) {
		if (stride == row_size) {
			memcpy(sc->pixel_buffer, mapped.pData, needed);
		} else {
			uint8_t *src = (uint8_t *)mapped.pData;
			uint8_t *dst = sc->pixel_buffer;
			for (int y = 0; y < sc->height; y++) {
				memcpy(dst, src, (size_t)row_size);
				src += stride;
				dst += row_size;
			}
		}
	}

	ID3D11DeviceContext_Unmap(sc->context, (ID3D11Resource *)sc->staging_texture, 0);

	sc->frame.data = sc->pixel_buffer;
	sc->frame.width = sc->width;
	sc->frame.height = sc->height;
	sc->frame.stride = row_size;

	return true;
}

bool screen_capture_get_monitor_name(int monitor_idx, char *name, size_t name_size)
{
	DISPLAY_DEVICEA dd;
	dd.cb = sizeof(dd);
	int count = 0;

	for (int adapter = 0; EnumDisplayDevicesA(NULL, adapter, &dd, 0); adapter++) {
		if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE))
			continue;

		for (int monitor = 0; ; monitor++) {
			DISPLAY_DEVICEA md;
			md.cb = sizeof(md);
			if (!EnumDisplayDevicesA(dd.DeviceName, monitor, &md, 1))
				break;
			if (!(md.StateFlags & DISPLAY_DEVICE_ACTIVE))
				continue;

			if (count == monitor_idx) {
				if (md.DeviceString[0]) {
					snprintf(name, name_size, "%s", md.DeviceString);
				} else {
					snprintf(name, name_size, "Monitor %d", monitor_idx + 1);
				}
				return true;
			}
			count++;
		}
	}

	snprintf(name, name_size, "Monitor %d", monitor_idx + 1);
	return false;
}

bool screen_capture_needs_recreate(struct screen_capture *sc)
{
	return sc && sc->access_lost;
}

bool screen_capture_recreate(struct screen_capture *sc)
{
	if (!sc) return false;

	obs_log(LOG_INFO, "Recreating screen capture for monitor %d...", sc->monitor_idx);

	// Release old duplication
	if (sc->duplication) {
		IDXGIOutputDuplication_ReleaseFrame(sc->duplication);
		IDXGIOutputDuplication_Release(sc->duplication);
		sc->duplication = NULL;
	}

	// Release staging texture so it gets recreated with new dimensions
	if (sc->staging_texture) {
		ID3D11Texture2D_Release(sc->staging_texture);
		sc->staging_texture = NULL;
	}

	// Release device and matched adapter
	if (sc->context) {
		ID3D11DeviceContext_Release(sc->context);
		sc->context = NULL;
	}
	if (sc->device) {
		ID3D11Device_Release(sc->device);
		sc->device = NULL;
	}
	if (sc->matched_adapter) {
		IDXGIAdapter_Release(sc->matched_adapter);
		sc->matched_adapter = NULL;
	}

	// Try all adapters on recreate (in case mode-switch changed which adapter owns the output)
	HRESULT hr = find_working_adapter_for_output(NULL, &sc->device, &sc->context, &sc->matched_adapter);
	if (FAILED(hr)) {
		// Fallback: try the standard device creation
		hr = create_d3d11_device(&sc->device, &sc->context);
		if (FAILED(hr)) {
			obs_log(LOG_ERROR, "Failed to recreate D3D11 device");
			return false;
		}
	}

	// Re-enumerate the target output
	IDXGIOutput *output = NULL;
	DXGI_OUTPUT_DESC output_desc;
	{
		IDXGIDevice *dxgi_device = NULL;
		IDXGIAdapter *adapter = NULL;
		hr = ID3D11Device_QueryInterface(sc->device, &IID_IDXGIDevice, (void **)&dxgi_device);
		if (SUCCEEDED(hr)) {
			hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
			if (SUCCEEDED(hr)) {
				hr = IDXGIAdapter_EnumOutputs(adapter, (UINT)sc->monitor_idx, &output);
				if (SUCCEEDED(hr)) {
					IDXGIOutput_GetDesc(output, &output_desc);
				}
				IDXGIAdapter_Release(adapter);
			}
			IDXGIDevice_Release(dxgi_device);
		}
		if (!output) {
			obs_log(LOG_ERROR, "Failed to enumerate monitor %d during recreate", sc->monitor_idx);
			return false;
		}
	}

	sc->width  = (int)(output_desc.DesktopCoordinates.right  - output_desc.DesktopCoordinates.left);
	sc->height = (int)(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);

	IDXGIOutput1 *output1 = NULL;
	hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
	if (SUCCEEDED(hr)) {
		hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)sc->device, &sc->duplication);
		IDXGIOutput1_Release(output1);
	}
	IDXGIOutput_Release(output);

	if (FAILED(hr) || !sc->duplication) {
		obs_log(LOG_ERROR, "DuplicateOutput failed during recreate: 0x%08lX", (unsigned long)hr);
		return false;
	}

	sc->access_lost = false;
	obs_log(LOG_INFO, "Screen capture recreated successfully for monitor %d (%dx%d)",
		sc->monitor_idx, sc->width, sc->height);
	return true;
}

struct capture_frame screen_capture_get_frame(struct screen_capture *sc)
{
	if (!sc) return (struct capture_frame){0};
	return sc->frame;
}

void screen_capture_get_size(struct screen_capture *sc, int *width, int *height)
{
	if (sc) {
		*width = sc->width;
		*height = sc->height;
	} else {
		*width = 0;
		*height = 0;
	}
}

int screen_capture_monitor_count(void)
{
	int count = GetSystemMetrics(SM_CMONITORS);
	return count > 0 ? count : 1;
}

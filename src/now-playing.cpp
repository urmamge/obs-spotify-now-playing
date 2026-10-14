/*
Spotify Now Playing for OBS
Copyright (C) 2026 urmamge

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#define NOMINMAX
#include <windows.h>
#include <Unknwn.h>
#include <objidl.h>
#include <shcore.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

using namespace winrt::Windows::Media::Control;
using winrt::Windows::Storage::Streams::IRandomAccessStreamReference;

/* Layout of Spotify's desktop player bar at 1x, measured from the app. */
namespace layout {
constexpr float height = 84.0f;
constexpr float art_x = 15.0f;
constexpr float art_y = 13.0f;
constexpr float art_size = 56.0f;
constexpr float text_x = 86.0f;
constexpr float title_y = 21.0f;
constexpr float artist_y = 44.0f;
constexpr float right_pad = 15.0f;
} // namespace layout

/* ------------------------------------------------------------------------- */
/* Media session watcher: one background thread shared by every source.      */

struct now_playing {
	bool active = false;
	bool playing = false;
	std::wstring title;
	std::wstring artist;
	std::vector<uint8_t> art; /* premultiplied BGRA */
	UINT art_w = 0;
	UINT art_h = 0;
};

static bool load_thumbnail(const IRandomAccessStreamReference &ref, now_playing &np)
{
	if (!ref)
		return false;

	auto ras = ref.OpenReadAsync().get();
	winrt::com_ptr<IStream> stream;
	if (FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(ras), IID_PPV_ARGS(stream.put()))))
		return false;

	std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromStream(stream.get()));
	if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok)
		return false;

	UINT w = bmp->GetWidth(), h = bmp->GetHeight();
	Gdiplus::Rect rect(0, 0, (INT)w, (INT)h);
	Gdiplus::BitmapData bd;
	if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
		return false;

	np.art.resize((size_t)w * h * 4);
	for (UINT y = 0; y < h; y++)
		memcpy(np.art.data() + (size_t)y * w * 4, (uint8_t *)bd.Scan0 + (ptrdiff_t)y * bd.Stride, (size_t)w * 4);
	np.art_w = w;
	np.art_h = h;
	bmp->UnlockBits(&bd);
	return true;
}

class media_watcher {
public:
	static media_watcher &get()
	{
		static media_watcher instance;
		return instance;
	}

	void acquire()
	{
		std::lock_guard<std::mutex> lock(life_mutex);
		if (refs++ == 0) {
			stopping = false;
			thread = std::thread(&media_watcher::run, this);
		}
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(life_mutex);
		if (refs > 0 && --refs == 0)
			stop();
	}

	void shutdown()
	{
		std::lock_guard<std::mutex> lock(life_mutex);
		refs = 0;
		stop();
	}

	uint64_t version() const { return ver.load(); }

	uint64_t snapshot(now_playing &out)
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		out = state;
		return ver.load();
	}

private:
	std::mutex life_mutex;
	int refs = 0;
	std::thread thread;

	std::mutex wait_mutex;
	std::condition_variable wake;
	bool stopping = false;

	std::mutex state_mutex;
	now_playing state;
	std::atomic<uint64_t> ver{1};

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(wait_mutex);
			stopping = true;
		}
		wake.notify_all();
		if (thread.joinable())
			thread.join();
	}

	void publish(now_playing &&np, bool art_updated)
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		if (!art_updated) {
			np.art = std::move(state.art);
			np.art_w = state.art_w;
			np.art_h = state.art_h;
		}
		bool changed = (art_updated && np.art != state.art) || np.active != state.active ||
			       np.playing != state.playing || np.title != state.title || np.artist != state.artist;
		state = std::move(np);
		if (changed)
			ver++;
	}

	void run()
	{
		winrt::init_apartment(winrt::apartment_type::multi_threaded);

		GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
		std::wstring last_track;
		int art_refreshes = 0;
		bool logged_error = false;

		for (;;) {
			try {
				if (!manager)
					manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				poll(manager, last_track, art_refreshes);
				logged_error = false;
			} catch (const winrt::hresult_error &e) {
				if (!logged_error)
					obs_log(LOG_WARNING, "media session error 0x%08X: %ls", (uint32_t)e.code(),
						e.message().c_str());
				logged_error = true;
				manager = nullptr;
			} catch (...) {
				manager = nullptr;
			}

			std::unique_lock<std::mutex> lock(wait_mutex);
			if (wake.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping; }))
				break;
		}

		manager = nullptr;
		winrt::uninit_apartment();
	}

	void poll(const GlobalSystemMediaTransportControlsSessionManager &manager, std::wstring &last_track,
		  int &art_refreshes)
	{
		GlobalSystemMediaTransportControlsSession session{nullptr};
		for (const auto &s : manager.GetSessions()) {
			std::wstring id{s.SourceAppUserModelId()};
			std::transform(id.begin(), id.end(), id.begin(), ::towlower);
			if (id.find(L"spotify") != std::wstring::npos) {
				session = s;
				break;
			}
		}

		now_playing np;
		if (!session) {
			last_track.clear();
			publish(std::move(np), true);
			return;
		}

		np.active = true;
		np.playing = session.GetPlaybackInfo().PlaybackStatus() ==
			     GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

		auto props = session.TryGetMediaPropertiesAsync().get();
		np.title = props.Title();
		np.artist = props.Artist();

		std::wstring track = np.title + L'\x1f' + np.artist;
		if (track != last_track) {
			last_track = track;
			/* Spotify often updates the thumbnail a moment after the title,
			 * so fetch it a few times after every track change. */
			art_refreshes = 4;
		}

		bool art_updated = false;
		if (art_refreshes > 0) {
			art_refreshes--;
			art_updated = true;
			if (!load_thumbnail(props.Thumbnail(), np)) {
				np.art.clear();
				np.art_w = np.art_h = 0;
			}
		}

		publish(std::move(np), art_updated);
	}
};

/* ------------------------------------------------------------------------- */
/* Source                                                                    */

struct now_playing_source {
	obs_source_t *source;

	std::mutex mutex;
	/* settings */
	int width = 384;
	float scale = 1.0f;
	std::wstring title_font, artist_font;
	float title_size = 15.0f, artist_size = 13.0f;
	uint32_t title_color = 0, artist_color = 0, bg_color = 0;
	int bg_radius = 0, art_radius = 4;
	bool hide_paused = false, hide_closed = true;

	uint64_t seen_version = 0;
	bool dirty = true;

	/* composed frame, waiting for upload on the graphics thread */
	std::vector<uint8_t> pixels;
	uint32_t cx = 0, cy = 0;
	bool upload = false;

	gs_texture_t *texture = nullptr;
	uint32_t tex_cx = 0, tex_cy = 0;
};

static Gdiplus::Color obs_to_gdip(uint32_t c, bool force_opaque)
{
	BYTE a = force_opaque ? 255 : (BYTE)(c >> 24);
	return Gdiplus::Color(a, (BYTE)(c & 0xFF), (BYTE)((c >> 8) & 0xFF), (BYTE)((c >> 16) & 0xFF));
}

static std::wstring to_wide(const char *utf8)
{
	wchar_t *w = nullptr;
	os_utf8_to_wcs_ptr(utf8 ? utf8 : "", 0, &w);
	std::wstring out = w ? w : L"";
	bfree(w);
	return out;
}

static void add_rounded_rect(Gdiplus::GraphicsPath &path, float x, float y, float w, float h, float r)
{
	r = std::min(r, std::min(w, h) / 2.0f);
	if (r <= 0.0f) {
		path.AddRectangle(Gdiplus::RectF(x, y, w, h));
		return;
	}
	float d = r * 2.0f;
	path.AddArc(x, y, d, d, 180.0f, 90.0f);
	path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
	path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
	path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
	path.CloseFigure();
}

static void draw_text(Gdiplus::Graphics &g, const std::wstring &text, const std::wstring &face, float size,
		      uint32_t color, float x, float y, float w)
{
	if (text.empty() || w <= 0.0f)
		return;

	Gdiplus::Font font(face.c_str(), size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	Gdiplus::Font fallback(L"Segoe UI", size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	const Gdiplus::Font *f = font.IsAvailable() ? &font : &fallback;

	Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
	fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsLineLimit);
	fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

	Gdiplus::SolidBrush brush(obs_to_gdip(color, true));
	g.DrawString(text.c_str(), (INT)text.size(), f, Gdiplus::RectF(x, y, w, size * 1.5f), &fmt, &brush);
}

static void compose(now_playing_source *s, const now_playing &np)
{
	const float scale = s->scale;
	const float width = (float)s->width;
	const uint32_t cx = (uint32_t)std::ceil(width * scale);
	const uint32_t cy = (uint32_t)std::ceil(layout::height * scale);

	Gdiplus::Bitmap bmp((INT)cx, (INT)cy, PixelFormat32bppPARGB);
	{
		Gdiplus::Graphics g(&bmp);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

		bool visible = np.active ? !(s->hide_paused && !np.playing) : !s->hide_closed;
		if (visible) {
			/* background */
			Gdiplus::GraphicsPath bg;
			add_rounded_rect(bg, 0.0f, 0.0f, width * scale, layout::height * scale, s->bg_radius * scale);
			Gdiplus::SolidBrush bg_brush(obs_to_gdip(s->bg_color, false));
			g.FillPath(&bg_brush, &bg);

			/* album art, resampled to its final size first so the
			 * rounded-corner fill below can sample it 1:1 */
			const float ax = layout::art_x * scale, ay = layout::art_y * scale;
			const INT asz = (INT)std::lround(layout::art_size * scale);
			Gdiplus::GraphicsPath art_path;
			add_rounded_rect(art_path, ax, ay, (float)asz, (float)asz, s->art_radius * scale);

			if (!np.art.empty()) {
				Gdiplus::Bitmap src((INT)np.art_w, (INT)np.art_h, (INT)np.art_w * 4, PixelFormat32bppPARGB,
						    (BYTE *)np.art.data());
				Gdiplus::Bitmap sized(asz, asz, PixelFormat32bppPARGB);
				{
					Gdiplus::Graphics sg(&sized);
					sg.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
					sg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
					Gdiplus::ImageAttributes attrs;
					attrs.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
					sg.DrawImage(&src, Gdiplus::Rect(0, 0, asz, asz), 0, 0, (INT)np.art_w,
						     (INT)np.art_h, Gdiplus::UnitPixel, &attrs);
				}
				Gdiplus::TextureBrush art_brush(&sized, Gdiplus::WrapModeClamp);
				art_brush.TranslateTransform(ax, ay);
				g.FillPath(&art_brush, &art_path);
			} else {
				Gdiplus::SolidBrush placeholder(Gdiplus::Color(255, 40, 40, 40));
				g.FillPath(&placeholder, &art_path);
			}

			/* text */
			g.ScaleTransform(scale, scale);
			const float text_w = width - layout::text_x - layout::right_pad;
			std::wstring title = np.active ? np.title : L"Spotify";
			std::wstring artist = np.active ? np.artist : L"Not running";
			draw_text(g, title, s->title_font, s->title_size, s->title_color, layout::text_x,
				  layout::title_y, text_w);
			draw_text(g, artist, s->artist_font, s->artist_size, s->artist_color, layout::text_x,
				  layout::artist_y, text_w);
		}
	}

	Gdiplus::Rect rect(0, 0, (INT)cx, (INT)cy);
	Gdiplus::BitmapData bd;
	if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
		return;
	s->pixels.resize((size_t)cx * cy * 4);
	for (uint32_t y = 0; y < cy; y++)
		memcpy(s->pixels.data() + (size_t)y * cx * 4, (uint8_t *)bd.Scan0 + (ptrdiff_t)y * bd.Stride,
		       (size_t)cx * 4);
	bmp.UnlockBits(&bd);

	s->cx = cx;
	s->cy = cy;
	s->upload = true;
}

static const char *np_get_name(void *)
{
	return obs_module_text("SpotifyNowPlaying");
}

static void np_update(void *data, obs_data_t *settings)
{
	auto s = static_cast<now_playing_source *>(data);
	std::lock_guard<std::mutex> lock(s->mutex);
	s->width = (int)obs_data_get_int(settings, "width");
	s->scale = (float)obs_data_get_double(settings, "scale");
	s->title_font = to_wide(obs_data_get_string(settings, "title_font"));
	s->title_size = (float)obs_data_get_double(settings, "title_size");
	s->title_color = (uint32_t)obs_data_get_int(settings, "title_color");
	s->artist_font = to_wide(obs_data_get_string(settings, "artist_font"));
	s->artist_size = (float)obs_data_get_double(settings, "artist_size");
	s->artist_color = (uint32_t)obs_data_get_int(settings, "artist_color");
	s->bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");
	s->bg_radius = (int)obs_data_get_int(settings, "bg_radius");
	s->art_radius = (int)obs_data_get_int(settings, "art_radius");
	s->hide_paused = obs_data_get_bool(settings, "hide_paused");
	s->hide_closed = obs_data_get_bool(settings, "hide_closed");
	s->dirty = true;
}

static void *np_create(obs_data_t *settings, obs_source_t *source)
{
	auto s = new now_playing_source();
	s->source = source;
	np_update(s, settings);
	media_watcher::get().acquire();
	return s;
}

static void np_destroy(void *data)
{
	auto s = static_cast<now_playing_source *>(data);
	media_watcher::get().release();
	obs_enter_graphics();
	gs_texture_destroy(s->texture);
	obs_leave_graphics();
	delete s;
}

static void np_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 384);
	obs_data_set_default_double(settings, "scale", 1.0);
	obs_data_set_default_string(settings, "title_font", "Segoe UI Semibold");
	obs_data_set_default_double(settings, "title_size", 15.0);
	obs_data_set_default_int(settings, "title_color", 0xFFFFFFFF);
	obs_data_set_default_string(settings, "artist_font", "Segoe UI Semibold");
	obs_data_set_default_double(settings, "artist_size", 13.0);
	obs_data_set_default_int(settings, "artist_color", 0xFFB3B3B3);
	obs_data_set_default_int(settings, "bg_color", 0xFF000000);
	obs_data_set_default_int(settings, "bg_radius", 0);
	obs_data_set_default_int(settings, "art_radius", 4);
	obs_data_set_default_bool(settings, "hide_paused", false);
	obs_data_set_default_bool(settings, "hide_closed", true);
}

static obs_properties_t *np_properties(void *)
{
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int_slider(p, "width", obs_module_text("Width"), 200, 1200, 1);
	obs_properties_add_float_slider(p, "scale", obs_module_text("Scale"), 0.25, 6.0, 0.01);
	obs_properties_add_text(p, "title_font", obs_module_text("TitleFont"), OBS_TEXT_DEFAULT);
	obs_properties_add_float(p, "title_size", obs_module_text("TitleSize"), 6.0, 72.0, 0.5);
	obs_properties_add_color(p, "title_color", obs_module_text("TitleColor"));
	obs_properties_add_text(p, "artist_font", obs_module_text("ArtistFont"), OBS_TEXT_DEFAULT);
	obs_properties_add_float(p, "artist_size", obs_module_text("ArtistSize"), 6.0, 72.0, 0.5);
	obs_properties_add_color(p, "artist_color", obs_module_text("ArtistColor"));
	obs_properties_add_color_alpha(p, "bg_color", obs_module_text("BackgroundColor"));
	obs_properties_add_int(p, "bg_radius", obs_module_text("BackgroundRadius"), 0, 42, 1);
	obs_properties_add_int(p, "art_radius", obs_module_text("ArtRadius"), 0, 28, 1);
	obs_properties_add_bool(p, "hide_paused", obs_module_text("HidePaused"));
	obs_properties_add_bool(p, "hide_closed", obs_module_text("HideClosed"));
	return p;
}

static uint32_t np_width(void *data)
{
	auto s = static_cast<now_playing_source *>(data);
	std::lock_guard<std::mutex> lock(s->mutex);
	return (uint32_t)std::ceil(s->width * s->scale);
}

static uint32_t np_height(void *data)
{
	auto s = static_cast<now_playing_source *>(data);
	std::lock_guard<std::mutex> lock(s->mutex);
	return (uint32_t)std::ceil(layout::height * s->scale);
}

static void np_tick(void *data, float)
{
	auto s = static_cast<now_playing_source *>(data);
	auto &watcher = media_watcher::get();

	std::lock_guard<std::mutex> lock(s->mutex);
	if (!s->dirty && watcher.version() == s->seen_version)
		return;

	now_playing np;
	s->seen_version = watcher.snapshot(np);
	s->dirty = false;
	compose(s, np);
}

static void np_render(void *data, gs_effect_t *effect)
{
	auto s = static_cast<now_playing_source *>(data);
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		if (s->upload) {
			const uint8_t *frame = s->pixels.data();
			if (!s->texture || s->tex_cx != s->cx || s->tex_cy != s->cy) {
				gs_texture_destroy(s->texture);
				s->texture = gs_texture_create(s->cx, s->cy, GS_BGRA, 1, &frame, GS_DYNAMIC);
				s->tex_cx = s->cx;
				s->tex_cy = s->cy;
			} else {
				gs_texture_set_image(s->texture, frame, s->cx * 4, false);
			}
			s->upload = false;
		}
	}
	if (!s->texture)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), s->texture);
	gs_draw_sprite(s->texture, 0, s->tex_cx, s->tex_cy);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

/* ------------------------------------------------------------------------- */

static ULONG_PTR gdiplus_token = 0;

extern "C" void now_playing_load(void)
{
	Gdiplus::GdiplusStartupInput input;
	Gdiplus::GdiplusStartup(&gdiplus_token, &input, nullptr);

	obs_source_info info = {};
	info.id = "spotify_now_playing";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = np_get_name;
	info.create = np_create;
	info.destroy = np_destroy;
	info.update = np_update;
	info.get_defaults = np_defaults;
	info.get_properties = np_properties;
	info.get_width = np_width;
	info.get_height = np_height;
	info.video_tick = np_tick;
	info.video_render = np_render;
	obs_register_source(&info);
}

extern "C" void now_playing_unload(void)
{
	media_watcher::get().shutdown();
	if (gdiplus_token)
		Gdiplus::GdiplusShutdown(gdiplus_token);
	gdiplus_token = 0;
}

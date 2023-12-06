#include <obs-module.h>
#include "version.h"
#include "md4c-html.h"
#include <util/dstr.h>
#include <util/threading.h>
#include <util/platform.h>
#include <sys/stat.h>

#define MARKDOWN_TEXT 0
#define MARKDOWN_FILE 1

#define STYLE_CSS 0
#define STYLE_CSS_FILE 1
#define STYLE_SETTINGS 2

struct markdown_source_data {
	obs_source_t *source;
	obs_source_t *browser;
	struct dstr html;
	struct dstr markdown_path;
	time_t markdown_time;
	struct dstr css_path;
	time_t css_time;
	pthread_t thread;
	bool stop;
	uint32_t sleep;
};

static char encoding_table[] = {
	'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
	'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
	'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
	'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};
static size_t mod_table[] = {0, 2, 1};

char *base64_encode(const unsigned char *data, size_t input_length,
		    size_t *output_length)
{

	*output_length = 4 * ((input_length + 2) / 3);

	char *encoded_data = bmalloc(*output_length + 1);
	if (encoded_data == NULL)
		return NULL;

	for (size_t i = 0, j = 0; i < input_length;) {

		uint32_t octet_a = i < input_length ? (unsigned char)data[i++]
						    : 0;
		uint32_t octet_b = i < input_length ? (unsigned char)data[i++]
						    : 0;
		uint32_t octet_c = i < input_length ? (unsigned char)data[i++]
						    : 0;

		uint32_t triple =
			(octet_a << 0x10) + (octet_b << 0x08) + octet_c;

		encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
		encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
		encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
		encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
	}

	for (size_t i = 0; i < mod_table[input_length % 3]; i++)
		encoded_data[*output_length - 1 - i] = '=';
	encoded_data[*output_length] = 0;
	return encoded_data;
}

static const char *markdown_source_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Markdown");
}

static void markdown_source_add_html(const MD_CHAR *tag, MD_SIZE size,
				     void *data)
{
	struct dstr *dstr = data;
	dstr_ncat(dstr, tag, size);
}

static void
markdown_source_set_browser_settings(struct markdown_source_data *md,
				     obs_data_t *settings, obs_data_t *bs)
{
	dstr_copy(&md->html, "<html><head><meta charset=\"UTF-8\"><script>\
window.addEventListener('setMarkdownHtml', function(event) { \
	document.body.innerHTML = event.detail.html;\
});\
window.addEventListener('setMarkdownCss', function(event) { \
	if (obsCSS) {\
		obsCSS.innerHTML = event.detail.css;\
	} else {\
		const obsCSS = document.createElement('style');\
		obsCSS.id = 'obsBrowserCustomStyle';\
		obsCSS.innerHTML = event.detail.css;\
		document.querySelector('head').appendChild(obsCSS);\
	}\
});\
</script></head><body>");
	const char *mdt = obs_data_get_string(settings, "text");
	md_html(mdt, (MD_SIZE)strlen(mdt), markdown_source_add_html, &md->html,
		MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS, 0);
	dstr_cat(&md->html, "</body></html>");

	size_t len;
	char *b64 = base64_encode((const unsigned char *)md->html.array,
				  md->html.len, &len);

	struct dstr url;
	dstr_init_copy(&url, "data:text/html;base64,");
	dstr_cat(&url, b64);
	obs_data_set_string(bs, "url", url.array);
	obs_data_set_string(bs, "css", obs_data_get_string(settings, "css"));
	dstr_free(&url);
	bfree(b64);
}

static void markdown_source_remove(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct markdown_source_data *md = data;
	if (!md->browser)
		return;
	obs_source_remove_active_child(md->source, md->browser);
	obs_source_release(md->browser);
	md->browser = NULL;
}

static bool markdown_source_file_changed(const char *path, time_t *time,
					 obs_data_t *settings,
					 const char *setting)
{
	bool changed = false;
	struct stat stats;
	if (os_stat(path, &stats) != 0)
		return changed;
	if (stats.st_mtime == *time)
		return changed;
	char *text = os_quick_read_utf8_file(path);
	if (!text)
		return changed;
	const char *old_text = obs_data_get_string(settings, setting);
	if (strcmp(text, old_text) != 0) {
		obs_data_set_string(settings, setting, text);
		changed = true;
		*time = stats.st_mtime;
	}
	bfree(text);
	return changed;
}

static void *markdown_source_thread(void *data)
{
	struct markdown_source_data *md = (struct markdown_source_data *)data;
	os_set_thread_name("markdown_source_thread");
	while (!md->stop) {
		os_sleep_ms(md->sleep);
		if (md->css_path.len == 0 && md->markdown_path.len == 0)
			continue;
		obs_data_t *settings = obs_source_get_settings(md->source);
		if (markdown_source_file_changed(md->markdown_path.array,
						 &md->markdown_time, settings,
						 "text") ||
		    markdown_source_file_changed(
			    md->css_path.array, &md->css_time, settings, "css"))
			obs_source_update(md->source, NULL);
		obs_data_release(settings);
	}
	return NULL;
}

static void *markdown_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct markdown_source_data *md =
		bzalloc(sizeof(struct markdown_source_data));
	md->source = source;
	md->sleep = 100;

	obs_data_t *bs = obs_data_create();
	obs_data_set_int(bs, "width", obs_data_get_int(settings, "width"));
	obs_data_set_int(bs, "height", obs_data_get_int(settings, "height"));

	dstr_init(&md->html);
	markdown_source_set_browser_settings(md, settings, bs);
	md->browser = obs_source_create_private("browser_source",
						"markdown browser", bs);
	obs_data_release(bs);
	obs_source_add_active_child(md->source, md->browser);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "remove", markdown_source_remove, md);

	if (obs_data_get_int(settings, "markdown_source") == MARKDOWN_FILE) {
		markdown_source_file_changed(
			obs_data_get_string(settings, "markdown_path"),
			&md->markdown_time, settings, "text");
	}
	if (obs_data_get_int(settings, "css_source") == STYLE_CSS_FILE) {
		markdown_source_file_changed(obs_data_get_string(settings,
								 "css_path"),
					     &md->css_time, settings, "css");
	}
	pthread_create(&md->thread, NULL, markdown_source_thread, md);

	return md;
}

static void markdown_source_destroy(void *data)
{
	struct markdown_source_data *md = data;
	dstr_free(&md->markdown_path);
	dstr_free(&md->css_path);
	md->stop = true;
	pthread_join(md->thread, NULL);
	if (md->browser) {
		obs_source_remove_active_child(md->source, md->browser);
		obs_source_release(md->browser);
	}
	dstr_free(&md->html);
	bfree(md);
}

uint32_t markdown_source_width(void *data)
{
	struct markdown_source_data *md = data;
	return obs_source_get_width(md->browser);
}

uint32_t markdown_source_height(void *data)
{
	struct markdown_source_data *md = data;
	return obs_source_get_height(md->browser);
}

void markdown_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct markdown_source_data *md = data;
	obs_source_video_render(md->browser);
}

static void markdown_source_enum_sources(void *data,
					 obs_source_enum_proc_t enum_callback,
					 void *param)
{
	struct markdown_source_data *md = data;
	if (md->browser)
		enum_callback(md->source, md->browser, param);
}

static void markdown_source_update(void *data, obs_data_t *settings)
{
	struct markdown_source_data *md = data;
	md->sleep = (uint32_t)obs_data_get_int(settings, "sleep");
	if (!md->sleep)
		md->sleep = 100;
	obs_data_t *bs = obs_source_get_settings(md->browser);
	if (obs_data_get_int(settings, "width") !=
		    obs_data_get_int(bs, "width") ||
	    obs_data_get_int(settings, "height") !=
		    obs_data_get_int(bs, "height")) {
		obs_data_set_int(bs, "width",
				 obs_data_get_int(settings, "width"));
		obs_data_set_int(bs, "height",
				 obs_data_get_int(settings, "height"));
		obs_source_update(md->browser, NULL);
	}
	if (obs_data_get_int(settings, "markdown_source") == MARKDOWN_FILE) {
		const char *path =
			obs_data_get_string(settings, "markdown_path");
		if (md->markdown_path.array == NULL ||
		    strcmp(md->markdown_path.array, path) != 0)
			dstr_copy(&md->markdown_path, path);
	} else if (md->markdown_path.len > 0) {
		dstr_copy(&md->markdown_path, "");
	}
	if (obs_data_get_bool(settings, "simple_style")) {
		obs_data_unset_user_value(settings, "simple_style");
		obs_data_set_int(settings, "css_source", STYLE_SETTINGS);
	}
	long long css_source = obs_data_get_int(settings, "css_source");
	if (css_source == STYLE_CSS_FILE) {
		const char *path = obs_data_get_string(settings, "css_path");
		if (md->css_path.array == NULL ||
		    strcmp(md->css_path.array, path) != 0)
			dstr_copy(&md->css_path, path);
	} else if (md->css_path.len > 0) {
		dstr_copy(&md->css_path, "");
	}
	if (css_source == STYLE_SETTINGS) {
		struct dstr css;
		dstr_init(&css);
		obs_data_t *font = obs_data_get_obj(settings, "font");
		long long bgcolor = obs_data_get_int(settings, "bgcolor");
		long long fgcolor = obs_data_get_int(settings, "fgcolor");
		dstr_printf(&css, "body { \n\
	background-color: rgba(%i, %i, %i, %i); \n\
	color: rgba(%i, %i, %i, %i);\n",
			    (int)(bgcolor & 0xff),
			    (int)((bgcolor / 0x100) & 0xff),
			    (int)((bgcolor / 0x10000) & 0xff),
			    (int)((bgcolor / 0x1000000) & 0xff),
			    (int)(fgcolor & 0xff),
			    (int)((fgcolor / 0x100) & 0xff),
			    (int)((fgcolor / 0x10000) & 0xff),
			    (int)((fgcolor / 0x1000000) & 0xff));
		if (font) {
			dstr_cat(&css, "\
	font-family: \"");
			dstr_cat(&css, obs_data_get_string(font, "face"));
			dstr_cat(&css, "\";\n");
			dstr_cat(&css, "\
	font-style: \"");
			dstr_cat(&css, obs_data_get_string(font, "style"));
			dstr_cat(&css, "\";\n");

			dstr_catf(&css, "\
	font-size: %i;\n",
				  (int)obs_data_get_int(font, "size"));
			obs_data_release(font);
		}
		dstr_cat(&css, "\
	margin: 0px 0px; \n\
	overflow: hidden; \n\
}");
		obs_data_set_string(settings, "css", css.array);
		dstr_free(&css);
	}
	const char *mdt = obs_data_get_string(settings, "text");
	dstr_copy(&md->html, " ");
	md_html(mdt, (MD_SIZE)strlen(mdt), markdown_source_add_html, &md->html,
		MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS, 0);
	bool refresh = false;
	proc_handler_t *ph = obs_source_get_proc_handler(md->browser);
	if (ph) {
		obs_data_t *json = obs_data_create();
		obs_data_set_string(json, "html", md->html.array);
		struct calldata cd = {0};
		calldata_set_string(&cd, "eventName", "setMarkdownHtml");
		calldata_set_string(&cd, "jsonString", obs_data_get_json(json));
		if (!proc_handler_call(ph, "javascript_event", &cd))
			refresh = true;
		obs_data_release(json);

		json = obs_data_create();
		obs_data_set_string(json, "css",
				    obs_data_get_string(settings, "css"));
		calldata_set_string(&cd, "eventName", "setMarkdownCss");
		calldata_set_string(&cd, "jsonString", obs_data_get_json(json));
		if (!proc_handler_call(ph, "javascript_event", &cd))
			refresh = true;
		calldata_free(&cd);
		obs_data_release(json);
	} else {
		refresh = true;
	}
	if (refresh) {
		markdown_source_set_browser_settings(md, settings, bs);
		obs_source_update(md->browser, NULL);
	}
	obs_data_release(bs);
}

static bool markdown_source_changed(void *data, obs_properties_t *props,
				    obs_property_t *property,
				    obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(property);

	bool markdown_is_file =
		(obs_data_get_int(settings, "markdown_source") ==
		 MARKDOWN_FILE);
	obs_property_t *p = obs_properties_get(props, "text");
	obs_property_set_visible(p, !markdown_is_file);
	p = obs_properties_get(props, "markdown_path");
	obs_property_set_visible(p, markdown_is_file);
	p = obs_properties_get(props, "sleep");
	obs_property_set_visible(p, markdown_is_file ||
					    obs_data_get_int(settings,
							     "css_source") ==
						    STYLE_CSS_FILE);
	return true;
}

static bool markdown_source_style_changed(void *data, obs_properties_t *props,
					  obs_property_t *property,
					  obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(property);
	long long style = obs_data_get_int(settings, "css_source");
	obs_property_t *p = obs_properties_get(props, "css");
	obs_property_set_visible(p, style == STYLE_CSS);
	p = obs_properties_get(props, "bgcolor");
	obs_property_set_visible(p, style == STYLE_SETTINGS);
	p = obs_properties_get(props, "fgcolor");
	obs_property_set_visible(p, style == STYLE_SETTINGS);
	p = obs_properties_get(props, "font");
	obs_property_set_visible(p, style == STYLE_SETTINGS);
	p = obs_properties_get(props, "css_path");
	obs_property_set_visible(p, style == STYLE_CSS_FILE);
	p = obs_properties_get(props, "sleep");
	obs_property_set_visible(
		p, style == STYLE_CSS_FILE ||
			   obs_data_get_int(settings, "markdown_source") ==
				   MARKDOWN_FILE);
	return true;
}

static obs_properties_t *markdown_source_properties(void *data)
{
	struct markdown_source_data *md = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "width", obs_module_text("Width"), 1,
			       8192, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 1,
			       8192, 1);
	obs_property_t *p = obs_properties_add_list(
		props, "markdown_source", obs_module_text("MarkdownSource"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Text"), MARKDOWN_TEXT);
	obs_property_list_add_int(p, obs_module_text("File"), MARKDOWN_FILE);
	obs_property_set_modified_callback2(p, markdown_source_changed, data);

	p = obs_properties_add_text(props, "text", obs_module_text("Markdown"),
				    OBS_TEXT_MULTILINE);
	obs_property_text_set_monospace(p, true);

	obs_properties_add_path(props, "markdown_path",
				obs_module_text("MarkdownFile"), OBS_PATH_FILE,
				"Markdown files (*.md);;All files (*.*)",
				md->markdown_path.array);

	p = obs_properties_add_list(props, "css_source",
				    obs_module_text("CssSource"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("CssText"), STYLE_CSS);
	obs_property_list_add_int(p, obs_module_text("CssFile"),
				  STYLE_CSS_FILE);
	obs_property_list_add_int(p, obs_module_text("SimpleStyle"),
				  STYLE_SETTINGS);
	obs_property_set_modified_callback2(p, markdown_source_style_changed,
					    data);

	obs_properties_add_color_alpha(props, "bgcolor",
				       obs_module_text("BackgroundColor"));
	obs_properties_add_color_alpha(props, "fgcolor",
				       obs_module_text("ForegroundColor"));
	obs_properties_add_font(props, "font", obs_module_text("Font"));

	p = obs_properties_add_text(props, "css", obs_module_text("CSS"),
				    OBS_TEXT_MULTILINE);
	obs_property_text_set_monospace(p, true);

	obs_properties_add_path(props, "css_path", obs_module_text("CssFile"),
				OBS_PATH_FILE,
				"CSS files (*.css);;All files (*.*)",
				md->css_path.array);

	p = obs_properties_add_int(props, "sleep", obs_module_text("Refresh"),
				   1, 10000, 1);
	obs_property_int_set_suffix(p, "ms");

	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/markdown-source.1764/\">Markdown Source</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return props;
}

static void markdown_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "css", "body { \n\
	background-color: rgba(0, 0, 0, 0); \n\
	color: rgba(255, 255, 255, 255); \n\
	margin: 0px 0px; \n\
	overflow: hidden; \n\
}");
	obs_data_set_default_int(settings, "width", 800);
	obs_data_set_default_int(settings, "height", 600);
	obs_data_set_default_int(settings, "sleep", 300);
	obs_data_set_default_int(settings, "bgcolor", 0);
	obs_data_set_default_int(settings, "fgcolor", 0xffffffff);
}

struct obs_source_info markdown_source = {
	.id = "markdown_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB,
	.icon_type = OBS_ICON_TYPE_TEXT,
	.create = markdown_source_create,
	.destroy = markdown_source_destroy,
	.update = markdown_source_update,
	.load = markdown_source_update,
	.get_name = markdown_source_name,
	.get_defaults = markdown_source_defaults,
	.get_width = markdown_source_width,
	.get_height = markdown_source_height,
	.video_render = markdown_source_render,
	.get_properties = markdown_source_properties,
	.enum_active_sources = markdown_source_enum_sources,
	.enum_all_sources = markdown_source_enum_sources,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("markdown", "en-US")

bool obs_module_load(void)
{
	blog(LOG_INFO, "[markdown] loaded version %s", PROJECT_VERSION);
	obs_register_source(&markdown_source);

	return true;
}

void obs_module_unload(void) {}

#include "markdown.h"
#include "md4c-html.h"
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QPainter>

static void markdown_source_add_html(const MD_CHAR *tag, MD_SIZE size, void *data)
{
	struct dstr *dstr = (struct dstr *)data;
	dstr_ncat(dstr, tag, size);
}

void render_qt(struct markdown_source_data *md, obs_data_t *settings)
{
	dstr_copy(&md->html, "<html>\n<head>\n<meta charset=\"UTF-8\">\n<style>");
	dstr_cat(&md->html, obs_data_get_string(settings, "css"));
	dstr_cat(&md->html, "</style>\n</head>\n<body>");
	const char *mdt = obs_data_get_string(settings, "text");
	md_html(mdt, (MD_SIZE)strlen(mdt), markdown_source_add_html, &md->html,
		MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS, 0);
	dstr_cat(&md->html, "</body></html>");

	QTextDocument td;
	td.setHtml(QString::fromUtf8(md->html.array));
	auto size = td.documentLayout()->documentSize();
	if (!md->texture || size != QSizeF(md->cx, md->cy)) {
		obs_enter_graphics();
		md->cx = (uint32_t)size.width();
		md->cy = (uint32_t)size.height();
		if (md->texture)
			gs_texture_destroy(md->texture);
		md->texture = gs_texture_create(md->cx, md->cy, GS_RGBA, 1, NULL, GS_DYNAMIC);
		obs_leave_graphics();
	}
	QImage image(md->cx, md->cy, QImage::Format_ARGB32_Premultiplied);
	QColor bgcolor = QColor::fromRgba(obs_data_get_int(settings, "bgcolor"));
	image.fill(bgcolor);
	QPainter painter(&image);
	painter.setPen(QColor::fromRgba(obs_data_get_int(settings, "fgcolor")));
	td.drawContents(&painter);
	painter.end();
	obs_enter_graphics();
	gs_texture_set_image(md->texture, image.constBits(), image.bytesPerLine(), false);
	obs_leave_graphics();
}

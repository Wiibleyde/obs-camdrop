#include "ui/dock.hpp"

#include "core/app.hpp"
#include "ui/settings-dialog.hpp"
#include "plugin-support.h"
#include "ui/third_party/browser-panel.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDesktopServices>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStackedLayout>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace camdrop::ui {

namespace {

QPointer<SupervisionDock> g_dock;

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

void create_dock()
{
	if (g_dock)
		return;
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dock = new SupervisionDock(main);
	if (!obs_frontend_add_dock_by_id("camdrop-supervision", obs_module_text("Dock.Title"), dock)) {
		delete dock;
		return;
	}
	g_dock = dock;
}

void shutdown()
{
	// CEF must release its browsers before OBS shuts it down.
	if (g_dock)
		g_dock->close_browser();
}

SupervisionDock::SupervisionDock(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(260, 160);
	stack_ = new QStackedLayout(this);
	stack_->setContentsMargins(0, 0, 0, 0);

	placeholder_ = new QWidget(this);
	auto *layout = new QVBoxLayout(placeholder_);
	message_ = new QLabel(placeholder_);
	message_->setWordWrap(true);
	message_->setAlignment(Qt::AlignCenter);
	auto *settings = new QPushButton(T("Dock.OpenSettings"), placeholder_);
	open_browser_ = new QPushButton(T("Dock.OpenInBrowser"), placeholder_);
	layout->addStretch();
	layout->addWidget(message_);
	layout->addWidget(settings);
	layout->addWidget(open_browser_);
	layout->addStretch();
	stack_->addWidget(placeholder_);

	connect(settings, &QPushButton::clicked, [] { open_settings(); });
	connect(open_browser_, &QPushButton::clicked,
		[] { QDesktopServices::openUrl(QUrl(QString::fromStdString(App::get().status().dock_url))); });

	cef_ = obs_browser_init_panel();
	if (!cef_)
		obs_log(LOG_INFO, "Supervision dock: obs-browser panel API not available (%s)",
			obs_get_module("obs-browser") ? "no QCef export" : "module not loaded");
	else
		cef_->init_browser(); // false only means "starting": refresh() waits for initialized()

	QPointer<SupervisionDock> self(this);
	listener_id_ = App::get().add_listener([self] {
		if (self)
			QMetaObject::invokeMethod(self, "refresh", Qt::QueuedConnection);
	});
	refresh();
}

SupervisionDock::~SupervisionDock()
{
	App::get().remove_listener(listener_id_);
}

void SupervisionDock::close_browser()
{
	if (browser_) {
		browser_->closeBrowser();
		stack_->setCurrentWidget(placeholder_);
		stack_->removeWidget(browser_);
		browser_->deleteLater();
		browser_ = nullptr;
		url_.clear();
	}
}

void SupervisionDock::refresh()
{
	const App::Status st = App::get().status();
	open_browser_->setVisible(st.running);

	if (!st.running) {
		message_->setText(
			T(st.error.empty() ? "Dock.ServerOff" : "Dock.ServerError") +
			(st.error.empty() ? QString() : QStringLiteral("\n") + QString::fromStdString(st.error)));
		stack_->setCurrentWidget(placeholder_);
		return;
	}

	if (!cef_) {
		message_->setText(T("Dock.NoBrowser"));
		stack_->setCurrentWidget(placeholder_);
		return;
	}

	if (!cef_->initialized()) {
		message_->setText(QStringLiteral("…"));
		stack_->setCurrentWidget(placeholder_);
		QTimer::singleShot(250, this, &SupervisionDock::refresh);
		return;
	}

	const char *locale = obs_get_locale();
	const std::string url = st.dock_url + "&embedded=1&lang=" + (locale ? locale : "en-US");
	if (!browser_) {
		browser_ = cef_->create_widget(this, url);
		if (!browser_) {
			obs_log(LOG_WARNING, "Supervision dock: cannot create the browser widget");
			message_->setText(T("Dock.NoBrowser"));
			stack_->setCurrentWidget(placeholder_);
			return;
		}
		stack_->addWidget(browser_);
		url_ = url;
	} else if (url != url_) {
		browser_->setURL(url);
		url_ = url;
	}
	stack_->setCurrentWidget(browser_);
}

} // namespace camdrop::ui

#include "ui/settings-dialog.hpp"

#include "core/app.hpp"
#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#if __has_include(<qrcodegencpp/qrcodegen.hpp>)
#include <qrcodegencpp/qrcodegen.hpp>
#else
#include <qrcodegen.hpp> // Debian / Ubuntu package
#endif

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QGuiApplication>
#include <QScreen>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace camdrop::ui {

namespace {

QPointer<SettingsDialog> g_dialog;

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QPixmap make_qr(const QString &text, int size)
{
	using qrcodegen::QrCode;
	const QrCode qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
	const int border = 2;
	const int n = qr.getSize() + border * 2;
	QImage img(n, n, QImage::Format_RGB32);
	img.fill(Qt::white);
	for (int y = 0; y < qr.getSize(); y++)
		for (int x = 0; x < qr.getSize(); x++)
			if (qr.getModule(x, y))
				img.setPixel(x + border, y + border, qRgb(0, 0, 0));
	return QPixmap::fromImage(img.scaled(size, size, Qt::KeepAspectRatio, Qt::FastTransformation));
}

} // namespace

void register_menu()
{
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Menu.Settings")));
	QObject::connect(action, &QAction::triggered, [] { open_settings(); });
}

void open_settings()
{
	if (!g_dialog) {
		auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
		g_dialog = new SettingsDialog(main);
		g_dialog->setAttribute(Qt::WA_DeleteOnClose);
	}
	g_dialog->show();
	g_dialog->raise();
	g_dialog->activateWindow();
}

void maybe_snapshot()
{
	const char *dir = getenv("CAMDROP_UI_SNAPSHOT");
	if (!dir || !*dir)
		return;
	const QString path = QString::fromLocal8Bit(dir);
	QTimer::singleShot(3000, [path] {
		open_settings();
		QTimer::singleShot(1500, [path] {
			if (g_dialog)
				g_dialog->grab().save(path + QStringLiteral("/settings.png"));
			auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
			if (main)
				main->grab().save(path + QStringLiteral("/main.png"));
			// Show the supervision dock floating, then capture the real screen (CEF draws natively).
			if (auto *dock = main ? main->findChild<QDockWidget *>(QStringLiteral("camdrop-supervision"))
					      : nullptr) {
				dock->setFloating(true);
				dock->resize(380, 520);
				dock->move(80, 80);
				dock->show();
				dock->raise();
				QTimer::singleShot(4000, [dock, path] {
					const QRect r = dock->frameGeometry();
					if (QScreen *screen = QGuiApplication::screenAt(r.center()))
						screen->grabWindow(0, r.x(), r.y(), r.width(), r.height())
							.save(path + QStringLiteral("/dock.png"));
				});
			}
		});
	});
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(T("Settings.Title"));
	setMinimumWidth(620);

	auto *root = new QVBoxLayout(this);

	enabled_ = new QCheckBox(T("Settings.Enable"), this);
	QFont bold = enabled_->font();
	bold.setBold(true);
	enabled_->setFont(bold);
	root->addWidget(enabled_);

	// Server
	auto *tabs = new QTabWidget(this);
	auto *server_box = new QWidget(tabs);
	auto *server_form = new QFormLayout(server_box);
	port_ = new QSpinBox(server_box);
	port_->setRange(1, 65535);
	server_form->addRow(T("Settings.Port"), port_);
	auto *udp_row = new QHBoxLayout();
	udp_min_ = new QSpinBox(server_box);
	udp_max_ = new QSpinBox(server_box);
	udp_min_->setRange(1024, 65535);
	udp_max_->setRange(1024, 65535);
	udp_row->addWidget(udp_min_);
	udp_row->addWidget(new QLabel(QStringLiteral("–"), server_box));
	udp_row->addWidget(udp_max_);
	udp_row->addStretch();
	server_form->addRow(T("Settings.UdpPorts"), udp_row);
	max_clients_ = new QSpinBox(server_box);
	max_clients_->setRange(1, 32);
	server_form->addRow(T("Settings.MaxClients"), max_clients_);
	auto *code_row = new QHBoxLayout();
	code_ = new QLineEdit(server_box);
	code_->setReadOnly(true);
	code_->setMaximumWidth(120);
	auto *regen_code = new QPushButton(T("Settings.RegenerateCode"), server_box);
	code_row->addWidget(code_);
	code_row->addWidget(regen_code);
	code_row->addStretch();
	server_form->addRow(T("Settings.AccessCode"), code_row);
	tabs->addTab(server_box, T("Settings.Server"));

	// Certificate
	auto *cert_box = new QWidget(tabs);
	auto *cert_layout = new QVBoxLayout(cert_box);
	auto *self_row = new QHBoxLayout();
	self_signed_ = new QRadioButton(T("Settings.SelfSigned"), cert_box);
	regen_cert_ = new QPushButton(T("Settings.RegenerateCert"), cert_box);
	self_row->addWidget(self_signed_);
	self_row->addStretch();
	self_row->addWidget(regen_cert_);
	cert_layout->addLayout(self_row);
	custom_cert_ = new QRadioButton(T("Settings.CustomCert"), cert_box);
	cert_layout->addWidget(custom_cert_);
	auto *cert_form = new QFormLayout();
	auto *cert_row = new QHBoxLayout();
	cert_path_ = new QLineEdit(cert_box);
	cert_browse_ = new QPushButton(T("Settings.Browse"), cert_box);
	cert_row->addWidget(cert_path_);
	cert_row->addWidget(cert_browse_);
	cert_form->addRow(T("Settings.CertFile"), cert_row);
	auto *key_row = new QHBoxLayout();
	key_path_ = new QLineEdit(cert_box);
	key_browse_ = new QPushButton(T("Settings.Browse"), cert_box);
	key_row->addWidget(key_path_);
	key_row->addWidget(key_browse_);
	cert_form->addRow(T("Settings.KeyFile"), key_row);
	cert_layout->addLayout(cert_form);
	cert_info_ = new QLabel(cert_box);
	cert_info_->setWordWrap(true);
	cert_info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	cert_layout->addWidget(cert_info_);
	tabs->addTab(cert_box, T("Settings.Certificate"));

	// Video requested from the cameras
	auto *video_box = new QWidget(tabs);
	auto *video_form = new QFormLayout(video_box);
	resolution_ = new QComboBox(video_box);
	resolution_->addItem(QStringLiteral("480p"), 480);
	resolution_->addItem(QStringLiteral("720p"), 720);
	resolution_->addItem(QStringLiteral("1080p"), 1080);
	video_form->addRow(T("Settings.Resolution"), resolution_);
	fps_ = new QComboBox(video_box);
	fps_->addItem(QStringLiteral("30 fps"), 30);
	fps_->addItem(QStringLiteral("60 fps"), 60);
	video_form->addRow(T("Settings.Fps"), fps_);
	bitrate_ = new QSpinBox(video_box);
	bitrate_->setRange(300, 50000);
	bitrate_->setSingleStep(500);
	bitrate_->setSuffix(QStringLiteral(" kbps"));
	video_form->addRow(T("Settings.MaxBitrate"), bitrate_);
	codec_ = new QComboBox(video_box);
	codec_->addItem(QStringLiteral("H.264"), QStringLiteral("h264"));
	codec_->addItem(QStringLiteral("VP8"), QStringLiteral("vp8"));
	codec_->addItem(T("Settings.Codec.H265"), QStringLiteral("h265"));
	video_form->addRow(T("Settings.Codec"), codec_);
	hw_decode_ = new QCheckBox(T("Settings.HwDecode"), video_box);
	video_form->addRow(QString(), hw_decode_);
	opus_10ms_ = new QCheckBox(T("Settings.Opus10ms"), video_box);
	video_form->addRow(QString(), opus_10ms_);
	orientation_ = new QComboBox(video_box);
	orientation_->addItem(T("Settings.Orientation.Any"), QStringLiteral("any"));
	orientation_->addItem(T("Settings.Orientation.Landscape"), QStringLiteral("landscape"));
	orientation_->addItem(T("Settings.Orientation.Portrait"), QStringLiteral("portrait"));
	video_form->addRow(T("Settings.Orientation"), orientation_);
	tabs->addTab(video_box, T("Settings.Video"));

	// Studio integration
	auto *studio_box = new QWidget(tabs);
	auto *studio_layout = new QVBoxLayout(studio_box);
	tally_ = new QCheckBox(T("Settings.Tally"), studio_box);
	studio_layout->addWidget(tally_);
	auto_add_ = new QCheckBox(T("Settings.AutoAdd"), studio_box);
	auto_add_->setToolTip(T("Settings.AutoAdd.Hint"));
	studio_layout->addWidget(auto_add_);
	auto *auto_add_hint = new QLabel(T("Settings.AutoAdd.Hint"), studio_box);
	auto_add_hint->setWordWrap(true);
	auto_add_hint->setStyleSheet(QStringLiteral("color: #8a8f99; margin-left: 22px;"));
	studio_layout->addWidget(auto_add_hint);
	studio_layout->addStretch();
	tabs->addTab(studio_box, T("Settings.Studio"));

	// How to connect
	connect_box_ = new QGroupBox(T("Settings.Connect"), this);
	auto *connect_layout = new QHBoxLayout(connect_box_);
	auto *connect_left = new QVBoxLayout();
	status_ = new QLabel(connect_box_);
	status_->setWordWrap(true);
	connect_left->addWidget(status_);
	urls_ = new QComboBox(connect_box_);
	connect_left->addWidget(urls_);
	auto *url_buttons = new QHBoxLayout();
	auto *copy = new QPushButton(T("Settings.CopyUrl"), connect_box_);
	auto *open = new QPushButton(T("Settings.OpenUrl"), connect_box_);
	url_buttons->addWidget(copy);
	url_buttons->addWidget(open);
	url_buttons->addStretch();
	connect_left->addLayout(url_buttons);
	auto *dock_label = new QLabel(T("Settings.DockUrl"), connect_box_);
	dock_label->setWordWrap(true);
	connect_left->addWidget(dock_label);
	auto *dock_row = new QHBoxLayout();
	dock_url_ = new QLineEdit(connect_box_);
	dock_url_->setReadOnly(true);
	auto *copy_dock = new QPushButton(T("Settings.Copy"), connect_box_);
	dock_row->addWidget(dock_url_);
	dock_row->addWidget(copy_dock);
	connect_left->addLayout(dock_row);
	connect_left->addStretch();
	connect_layout->addLayout(connect_left, 1);
	qr_ = new QLabel(connect_box_);
	qr_->setFixedSize(180, 180);
	qr_->setAlignment(Qt::AlignCenter);
	connect_layout->addWidget(qr_);
	root->addWidget(connect_box_);
	root->addWidget(tabs);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
	root->addWidget(buttons);

	connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::apply);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
	connect(regen_code, &QPushButton::clicked, this, &SettingsDialog::regenerate_code);
	connect(regen_cert_, &QPushButton::clicked, this, &SettingsDialog::regenerate_certificate);
	connect(cert_browse_, &QPushButton::clicked, this, &SettingsDialog::browse_cert);
	connect(key_browse_, &QPushButton::clicked, this, &SettingsDialog::browse_key);
	connect(copy, &QPushButton::clicked, this, &SettingsDialog::copy_url);
	connect(open, &QPushButton::clicked, this, &SettingsDialog::open_url);
	connect(copy_dock, &QPushButton::clicked, this, &SettingsDialog::copy_dock_url);
	connect(urls_, &QComboBox::currentIndexChanged, this, &SettingsDialog::update_qr);
	connect(self_signed_, &QRadioButton::toggled, this, &SettingsDialog::update_enabled_state);
	connect(enabled_, &QCheckBox::toggled, this, &SettingsDialog::update_enabled_state);

	load_from_settings();
	refresh_status();

	QPointer<SettingsDialog> self(this);
	listener_id_ = App::get().add_listener([self] {
		if (self)
			QMetaObject::invokeMethod(self, "refresh_status", Qt::QueuedConnection);
	});
}

SettingsDialog::~SettingsDialog()
{
	App::get().remove_listener(listener_id_);
}

void SettingsDialog::load_from_settings()
{
	const Settings s = App::get().settings();
	enabled_->setChecked(s.server_enabled);
	port_->setValue(s.port);
	udp_min_->setValue(s.udp_port_min);
	udp_max_->setValue(s.udp_port_max);
	max_clients_->setValue(s.max_clients);
	code_->setText(QString::fromStdString(s.access_code));
	self_signed_->setChecked(!s.custom_cert);
	custom_cert_->setChecked(s.custom_cert);
	cert_path_->setText(QString::fromStdString(s.cert_path));
	key_path_->setText(QString::fromStdString(s.key_path));
	int idx = resolution_->findData(s.video_height);
	resolution_->setCurrentIndex(idx >= 0 ? idx : 1);
	idx = fps_->findData(s.video_fps);
	fps_->setCurrentIndex(idx >= 0 ? idx : 0);
	bitrate_->setValue(s.max_bitrate_kbps);
	idx = codec_->findData(QString::fromStdString(s.preferred_codec));
	codec_->setCurrentIndex(idx >= 0 ? idx : 0);
	hw_decode_->setChecked(s.hw_decode);
	opus_10ms_->setChecked(s.opus_10ms);
	idx = orientation_->findData(QString::fromStdString(s.orientation));
	orientation_->setCurrentIndex(idx >= 0 ? idx : 0);
	tally_->setChecked(s.tally);
	auto_add_->setChecked(s.auto_add_sources);
	update_enabled_state();
}

void SettingsDialog::update_enabled_state()
{
	const bool custom = custom_cert_->isChecked();
	cert_path_->setEnabled(custom);
	key_path_->setEnabled(custom);
	cert_browse_->setEnabled(custom);
	key_browse_->setEnabled(custom);
	regen_cert_->setEnabled(!custom);
}

void SettingsDialog::apply()
{
	Settings s = App::get().settings();
	s.server_enabled = enabled_->isChecked();
	s.port = port_->value();
	s.udp_port_min = std::min(udp_min_->value(), udp_max_->value());
	s.udp_port_max = std::max(udp_min_->value(), udp_max_->value());
	s.max_clients = max_clients_->value();
	s.access_code = code_->text().toStdString();
	s.custom_cert = custom_cert_->isChecked();
	s.cert_path = cert_path_->text().trimmed().toStdString();
	s.key_path = key_path_->text().trimmed().toStdString();
	s.video_height = resolution_->currentData().toInt();
	s.video_fps = fps_->currentData().toInt();
	s.max_bitrate_kbps = bitrate_->value();
	s.preferred_codec = codec_->currentData().toString().toStdString();
	s.hw_decode = hw_decode_->isChecked();
	s.opus_10ms = opus_10ms_->isChecked();
	s.orientation = orientation_->currentData().toString().toStdString();
	s.tally = tally_->isChecked();
	s.auto_add_sources = auto_add_->isChecked();

	std::string error;
	if (!App::get().apply(s, error)) {
		QMessageBox::warning(this, T("Settings.Title"),
				     T("Settings.StartFailed") + QStringLiteral("\n\n") +
					     QString::fromStdString(error));
	}
	refresh_status();
}

void SettingsDialog::refresh_status()
{
	const App::Status st = App::get().status();
	const Settings s = App::get().settings();
	code_->setText(QString::fromStdString(s.access_code));

	if (st.running) {
		status_->setText(T("Settings.StatusRunning").arg(st.port).arg(st.clients));
		status_->setStyleSheet(QString());
	} else if (!st.error.empty()) {
		status_->setText(T("Settings.StatusError") + QStringLiteral(" ") + QString::fromStdString(st.error));
		status_->setStyleSheet(QStringLiteral("color: #e5484d;"));
	} else {
		status_->setText(T("Settings.StatusStopped"));
		status_->setStyleSheet(QString());
	}

	const QString current = urls_->currentText();
	urls_->blockSignals(true);
	urls_->clear();
	for (const auto &u : st.camera_urls)
		urls_->addItem(QString::fromStdString(u));
	const int idx = urls_->findText(current);
	urls_->setCurrentIndex(idx >= 0 ? idx : 0);
	urls_->blockSignals(false);
	urls_->setEnabled(st.running);
	dock_url_->setText(QString::fromStdString(st.dock_url));
	cert_info_->setText(st.cert_summary.empty() ? QString()
						    : T("Settings.CertInfo") + QStringLiteral(" ") +
							      QString::fromStdString(st.cert_summary));
	update_qr();
}

void SettingsDialog::update_qr()
{
	const App::Status st = App::get().status();
	if (!st.running || urls_->count() == 0) {
		qr_->setPixmap(QPixmap());
		qr_->setText(T("Settings.QrUnavailable"));
		qr_->setWordWrap(true);
		return;
	}
	qr_->setPixmap(make_qr(urls_->currentText(), qr_->width()));
}

void SettingsDialog::regenerate_code()
{
	if (QMessageBox::question(this, T("Settings.Title"), T("Settings.RegenerateCodeConfirm")) != QMessageBox::Yes)
		return;
	code_->setText(QString::fromStdString(generate_access_code()));
	apply();
}

void SettingsDialog::regenerate_certificate()
{
	if (QMessageBox::question(this, T("Settings.Title"), T("Settings.RegenerateCertConfirm")) != QMessageBox::Yes)
		return;
	std::string error;
	if (!App::get().regenerate_certificate(error))
		QMessageBox::warning(this, T("Settings.Title"), QString::fromStdString(error));
	refresh_status();
}

void SettingsDialog::browse_cert()
{
	const QString path = QFileDialog::getOpenFileName(this, T("Settings.CertFile"), cert_path_->text(),
							  QStringLiteral("PEM (*.pem *.crt *.cer);;* (*)"));
	if (!path.isEmpty())
		cert_path_->setText(path);
}

void SettingsDialog::browse_key()
{
	const QString path = QFileDialog::getOpenFileName(this, T("Settings.KeyFile"), key_path_->text(),
							  QStringLiteral("PEM (*.pem *.key);;* (*)"));
	if (!path.isEmpty())
		key_path_->setText(path);
}

void SettingsDialog::copy_url()
{
	if (urls_->count())
		QApplication::clipboard()->setText(urls_->currentText());
}

void SettingsDialog::open_url()
{
	if (urls_->count())
		QDesktopServices::openUrl(QUrl(urls_->currentText()));
}

void SettingsDialog::copy_dock_url()
{
	QApplication::clipboard()->setText(dock_url_->text());
}

} // namespace camdrop::ui

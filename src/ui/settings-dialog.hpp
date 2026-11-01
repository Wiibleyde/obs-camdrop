#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QGroupBox;

namespace camdrop::ui {

void register_menu();
void open_settings();
void shutdown();
// Test hook: CAMDROP_UI_SNAPSHOT=<dir> saves pictures of the settings dialog after start-up.
void maybe_snapshot();

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(QWidget *parent);
	~SettingsDialog() override;

private slots:
	void apply();
	void refresh_status();
	void update_qr();
	void regenerate_code();
	void regenerate_certificate();
	void browse_cert();
	void browse_key();
	void copy_url();
	void copy_dock_url();
	void open_url();
	void update_enabled_state();

private:
	void load_from_settings();

	QCheckBox *enabled_;
	QSpinBox *port_;
	QSpinBox *udp_min_;
	QSpinBox *udp_max_;
	QSpinBox *max_clients_;
	QLineEdit *code_;
	QRadioButton *self_signed_;
	QRadioButton *custom_cert_;
	QLineEdit *cert_path_;
	QLineEdit *key_path_;
	QPushButton *cert_browse_;
	QPushButton *key_browse_;
	QPushButton *regen_cert_;
	QLabel *cert_info_;
	QComboBox *resolution_;
	QComboBox *fps_;
	QSpinBox *bitrate_;
	QComboBox *codec_;
	QCheckBox *hw_decode_;
	QCheckBox *opus_10ms_;
	QLabel *status_;
	QComboBox *urls_;
	QLabel *qr_;
	QLineEdit *dock_url_;
	QGroupBox *connect_box_;
	int listener_id_ = 0;
};

} // namespace camdrop::ui

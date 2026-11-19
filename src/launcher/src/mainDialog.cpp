#include "mainDialog.h"

#include "cheatFile.h"
#include "configuration.h"
#include "configurationItem.h"
#include "configurationListWidget.h"
#include "controllerPreview.h"
#include "gameContent.h"
#include "titleSound.h"
#include "updateChecker.h"

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QStyle>
#include <QStyleHints>
#include <QToolButton>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QtCore>

#include <cstdint>
#include <memory>

#include "ui_main_dialog.h"

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

class QWidget;

#if defined(_WIN32)
constexpr char  EMULATOR_EXE[] = "kyty_emulator.exe";
constexpr char  CMD_EXE[]      = "cmd.exe";
constexpr DWORD CMD_X_CHARS    = 175;
constexpr DWORD CMD_Y_CHARS    = 1000;
#else
constexpr char EMULATOR_EXE[] = "kyty_emulator";
#endif
constexpr char SETTINGS_MAIN_DIALOG[]        = "MainDialog";
constexpr char SETTINGS_MAIN_LAST_GEOMETRY[] = "geometry";
constexpr char SETTINGS_CHECK_UPDATES[]      = "check_updates_on_startup";
constexpr char SETTINGS_CLOSE_TERMINAL[]     = "close_terminal";
constexpr char SETTINGS_THEME[]              = "theme";
constexpr char SETTINGS_ARTWORK[]            = "artwork_path";

namespace {

constexpr QSize ArtworkPreviewSize {224, 126};
constexpr auto OfficialArtwork =
    "https://www.rockstargames.com/VI/_next/static/media/"
    "Official_Cover_Art_landscape.12.uu2irr.2_a.jpg?akim=1&imdensity=1&imwidth=3840";
constexpr qsizetype MaxOfficialArtworkBytes = 4 * 1024 * 1024;

QImage ReadArtwork(QImageReader& reader) {
	const auto size = reader.size();
	if (!size.isValid() || size.width() > 8192 || size.height() > 8192 ||
	    qint64(size.width()) * size.height() > 32 * 1024 * 1024) {
		return {};
	}
	reader.setAutoTransform(true);
	reader.setScaledSize(size.boundedTo(ArtworkPreviewSize) == size
	                         ? size
	                         : size.scaled(ArtworkPreviewSize, Qt::KeepAspectRatio));
	return reader.read();
}

QImage ReadLocalArtwork(const QString& path) {
	const QFileInfo file(path);
	if (!file.isFile() || file.size() > qint64(GameContent::MaxImageSize)) {
		return {};
	}
	QImageReader reader(path);
	return ReadArtwork(reader);
}

QImage ReadGameArtwork(const QString& base) {
	for (const auto* relative: {"sce_sys/pic0.png", "sce_sys/icon0.png"}) {
		auto data = GameContent::ReadFile(base, QString::fromLatin1(relative), GameContent::MaxImageSize);
		QBuffer buffer(&data);
		buffer.open(QIODevice::ReadOnly);
		QImageReader reader(&buffer);
		if (auto image = ReadArtwork(reader); !image.isNull()) {
			return image;
		}
	}
	return {};
}

} // namespace

class MainDialogPrivate: public QObject {
	Q_OBJECT

public:
	explicit MainDialogPrivate(QObject* parent = nullptr): QObject(parent) {}
	~MainDialogPrivate() override;

	void Setup(MainDialog* main_dialog);

	/*slots:*/

	void Update();
	void FindInterpreter();
	void Run(const QString& activity, const QString& benchmark_output = {});

	[[nodiscard]] const QString& GetInterpreter() const { return m_interpreter; }

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

private:
	void CreateLaunchPanel();
	void ChooseArtwork();
	void UpdateArtwork(const ConfigurationItem* item);
	void LoadOfficialArtwork();
	void ShowArtwork(const QImage& image, bool official);
	void ChooseBenchmarkOutput();
	static QByteArray g_last_geometry;
	static bool       g_check_updates_on_startup;
	static bool       g_close_terminal;
	static int        g_theme;
	static QString    g_artwork_path;

	static void ApplyTheme(int theme) {
		auto scheme = Qt::ColorScheme::Unknown;
		switch (theme) {
			case 1:
			case 3: scheme = Qt::ColorScheme::Dark; break;
			case 2: scheme = Qt::ColorScheme::Light; break;
			default: break;
		}
		QApplication::styleHints()->setColorScheme(scheme);
	}

	Ui::MainDialog* m_ui             = {nullptr};
	MainDialog*     m_main_dialog    = nullptr;
	UpdateChecker*  m_update_checker = nullptr;
	ControllerPreview m_controller_preview;
	TitleSound      m_title_sound;
	QString         m_interpreter;
	QLabel*         m_selection = nullptr;
	QLabel*         m_artwork = nullptr;
	QString         m_last_artwork_path;
	QString         m_last_artwork_game;
	QImage          m_official_artwork;
	bool            m_artwork_initialized = false;
	bool            m_official_artwork_requested = false;
	QPushButton*    m_launch = nullptr;
	QPushButton*    m_settings = nullptr;
	QPushButton*    m_benchmark = nullptr;

	int m_running_games = 0;
};

QByteArray MainDialogPrivate::g_last_geometry;
bool       MainDialogPrivate::g_check_updates_on_startup = true;
bool       MainDialogPrivate::g_close_terminal           = false;
int        MainDialogPrivate::g_theme                    = 3;
QString    MainDialogPrivate::g_artwork_path;

MainDialog::MainDialog(QWidget* parent): QDialog(parent), m_p(new MainDialogPrivate(this)) {
	m_p->Setup(this);
}

MainDialogPrivate::~MainDialogPrivate() {
	delete m_ui;
}

void MainDialogPrivate::Setup(MainDialog* main_dialog) {
	m_ui = new Ui::MainDialog;
	m_ui->setupUi(main_dialog);
	m_ui->widget->SetControllerPreview(&m_controller_preview);

	m_main_dialog = main_dialog;
	CreateLaunchPanel();
	m_update_checker = new UpdateChecker(main_dialog);
	m_ui->check_updates_on_startup->setChecked(g_check_updates_on_startup);
	m_ui->close_terminal->setChecked(g_close_terminal);
	m_ui->theme_combo->setCurrentIndex(g_theme);
	ApplyTheme(g_theme);
	m_main_dialog->setProperty("graphiteTheme", g_theme == 3);
	m_ui->label_Interpreter->hide();
	m_ui->label_settings_file->hide();
	m_ui->check_updates_link->setVisible(UpdateChecker::IsSupported());
	m_ui->check_updates_on_startup->setVisible(UpdateChecker::IsSupported());
#if defined(__APPLE__)
	m_ui->close_terminal->setVisible(false); // The emulator runs without a terminal.
#endif

	main_dialog->setWindowFlags(Qt::Dialog | Qt::WindowMinMaxButtonsHint |
	                            Qt::WindowCloseButtonHint);

	connect(main_dialog, &MainDialog::Start, this, &MainDialogPrivate::FindInterpreter,
	        Qt::QueuedConnection);
	connect(m_ui->widget, &ConfigurationListWidget::Select, this, [this]() {
		Update();
		const auto* item = m_ui->widget->GetSelectedItem();
		m_title_sound.SetTitle(m_running_games == 0 && item != nullptr ? item->GetInfo().basedir
		                                                               : QString());
	});
	connect(m_ui->widget, &ConfigurationListWidget::PreviewControllerColor, this,
	        [this](const QString& color) {
		        if (m_running_games == 0) {
			        m_controller_preview.SetColor(color);
		        }
	        });
	connect(m_ui->widget, &ConfigurationListWidget::Run, this,
	        [this](const QString& activity) { Run(activity); });
	connect(m_ui->check_updates_link, &QLabel::linkActivated, this,
	        [this](const QString&) { m_update_checker->Check(true); });
	connect(m_update_checker, &UpdateChecker::CheckingChanged, m_ui->check_updates_link,
	        &QLabel::setDisabled);
	connect(m_update_checker, &UpdateChecker::InstallingChanged, this, &MainDialogPrivate::Update);
	connect(m_ui->check_updates_on_startup, &QCheckBox::toggled, this, [this](bool checked) {
		g_check_updates_on_startup = checked;
		m_ui->widget->WriteSettings();
	});
	connect(m_ui->close_terminal, &QCheckBox::toggled, this, [this](bool checked) {
		g_close_terminal = checked;
		m_ui->widget->WriteSettings();
	});
	connect(m_ui->theme_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
		g_theme = index;
		ApplyTheme(index);
		m_main_dialog->setProperty("graphiteTheme", index == 3);
		m_main_dialog->style()->unpolish(m_main_dialog);
		m_main_dialog->style()->polish(m_main_dialog);
		m_main_dialog->update();
		m_ui->widget->WriteSettings();
	});
	connect(main_dialog, &MainDialog::Resize, [this]() {
		g_last_geometry = m_main_dialog->saveGeometry();
		m_ui->widget->WriteSettings();
	});

	m_ui->label_settings_file->setText(tr("Settings file: ") + m_ui->widget->GetSettingsFile());

	m_main_dialog->restoreGeometry(g_last_geometry);

	Update();
}

void MainDialogPrivate::CreateLaunchPanel() {
	auto* panel = new QFrame(m_main_dialog);
	panel->setObjectName(QStringLiteral("launchPanel"));
	m_ui->verticalLayout_5->insertWidget(0, panel);
	m_ui->verticalLayout_5->setSpacing(10);
	auto* row = new QHBoxLayout(panel);
	row->setContentsMargins(18, 14, 14, 14);
	row->setSpacing(16);
	auto* stack = new QVBoxLayout;
	stack->setSpacing(7);
	row->addLayout(stack, 1);

	auto* top = new QHBoxLayout;
	auto* heading = new QLabel(tr("KytyPs5 GTA VI"), panel);
	heading->setObjectName(QStringLiteral("launchHeading"));
	top->addWidget(heading);
	top->addStretch();
	auto* artwork = new QToolButton(panel);
	artwork->setText(tr("Artwork"));
	artwork->setPopupMode(QToolButton::InstantPopup);
	auto* menu = new QMenu(artwork);
	menu->addAction(tr("Choose local image…"), this, &MainDialogPrivate::ChooseArtwork);
	menu->addAction(tr("Use artwork from game"), this, [this] {
		g_artwork_path.clear();
		m_ui->widget->WriteSettings();
		UpdateArtwork(m_ui->widget->GetSelectedItem());
	});
	menu->addSeparator();
	menu->addAction(tr("Official Rockstar artwork"), this, [] {
		QDesktopServices::openUrl(
		    QUrl(QStringLiteral("https://www.rockstargames.com/VI/media/artwork-wallpapers")));
	});
	artwork->setMenu(menu);
	top->addWidget(artwork);
	stack->addLayout(top);

	auto* status = new QLabel(tr("GTA VI compatibility is untested"), panel);
	status->setObjectName(QStringLiteral("launchStatus"));
	stack->addWidget(status, 0, Qt::AlignLeft);

	m_selection = new QLabel(panel);
	m_selection->setObjectName(QStringLiteral("launchSelection"));
	m_selection->setTextFormat(Qt::PlainText);
	m_selection->setWordWrap(true);
	stack->addWidget(m_selection);

	auto make_button = [panel](const QString& label) {
		auto* button = new QPushButton(label, panel);
		button->setAutoDefault(false);
		return button;
	};
	m_launch = make_button(tr("Launch selected"));
	m_launch->setObjectName(QStringLiteral("launchPrimary"));
	m_settings = make_button(tr("Game settings"));
	m_benchmark = make_button(tr("Benchmark"));
	m_benchmark->setToolTip(tr("Capture frame timing and resource counters for up to 120 seconds"));
	auto* folders = make_button(tr("Add game folder"));
	auto* actions = new QHBoxLayout;
	actions->setSpacing(7);
	actions->addWidget(m_launch);
	actions->addWidget(m_settings);
	actions->addWidget(m_benchmark);
	actions->addWidget(folders);
	actions->addStretch();
	stack->addLayout(actions);

	m_artwork = new QLabel(panel);
	m_artwork->setFixedSize(ArtworkPreviewSize);
	m_artwork->setAlignment(Qt::AlignCenter);
	m_artwork->setAccessibleName(tr("Game artwork"));
	m_artwork->hide();
	row->addWidget(m_artwork, 0, Qt::AlignVCenter);

	connect(m_launch, &QPushButton::clicked, this, [this] { Run({}); });
	connect(m_settings, &QPushButton::clicked, m_ui->widget,
	        &ConfigurationListWidget::OpenSelectedGameSettings);
	connect(folders, &QPushButton::clicked, m_ui->widget, &ConfigurationListWidget::AddGameFolder);
	connect(m_benchmark, &QPushButton::clicked, this, &MainDialogPrivate::ChooseBenchmarkOutput);
}

void MainDialogPrivate::ChooseArtwork() {
	const auto path = QFileDialog::getOpenFileName(
	    m_main_dialog, tr("Choose artwork"), g_artwork_path,
	    tr("Images (*.jpg *.jpeg *.png *.webp)"));
	if (path.isEmpty()) {
		return;
	}
	if (ReadLocalArtwork(path).isNull()) {
		QMessageBox::warning(m_main_dialog, tr("Artwork"), tr("This image could not be opened."));
		return;
	}
	g_artwork_path = path;
	m_last_artwork_path.clear();
	m_ui->widget->WriteSettings();
	UpdateArtwork(m_ui->widget->GetSelectedItem());
}

void MainDialogPrivate::UpdateArtwork(const ConfigurationItem* item) {
	const auto game = item != nullptr && item->GetInfo().IsGtaVi() ? item->GetInfo().basedir : QString();
	if (m_artwork_initialized && m_last_artwork_path == g_artwork_path &&
	    m_last_artwork_game == game) {
		return;
	}
	m_artwork_initialized = true;
	m_last_artwork_path = g_artwork_path;
	m_last_artwork_game = game;
	auto image = g_artwork_path.isEmpty() ? QImage() : ReadLocalArtwork(g_artwork_path);
	if (image.isNull() && !game.isEmpty()) {
		image = ReadGameArtwork(game);
	}
	if (!image.isNull()) {
		ShowArtwork(image, false);
		return;
	}
	ShowArtwork(m_official_artwork, true);
	if (m_official_artwork.isNull() &&
	    !QCoreApplication::arguments().contains(QStringLiteral("--local"))) {
		LoadOfficialArtwork();
	}
}

void MainDialogPrivate::ShowArtwork(const QImage& image, bool official) {
	m_artwork->setPixmap(QPixmap::fromImage(image));
	const auto name = official ? tr("Official GTA VI cover art by Rockstar Games")
	                           : tr("Local game artwork");
	m_artwork->setAccessibleName(name);
	m_artwork->setToolTip(name);
	m_artwork->setVisible(!image.isNull());
}

void MainDialogPrivate::LoadOfficialArtwork() {
	if (m_official_artwork_requested) {
		return;
	}
	m_official_artwork_requested = true;
	auto* network = new QNetworkAccessManager(this);
	QNetworkRequest request {QUrl(QString::fromLatin1(OfficialArtwork))};
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(8000);
	auto* reply = network->get(request);
	reply->setReadBufferSize(64 * 1024);
	auto bytes = std::make_shared<QByteArray>();
	connect(reply, &QIODevice::readyRead, this, [reply, bytes] {
		bytes->append(reply->read(MaxOfficialArtworkBytes - bytes->size() + 1));
		if (bytes->size() > MaxOfficialArtworkBytes) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, network, reply, bytes] {
		network->deleteLater();
		if (reply->error() != QNetworkReply::NoError ||
		    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
		    bytes->size() > MaxOfficialArtworkBytes) {
			return;
		}
		bytes->append(reply->read(MaxOfficialArtworkBytes - bytes->size() + 1));
		if (bytes->size() > MaxOfficialArtworkBytes) {
			return;
		}
		QBuffer buffer(bytes.get());
		buffer.open(QIODevice::ReadOnly);
		QImageReader reader(&buffer, "jpeg");
		m_official_artwork = ReadArtwork(reader);
		if (m_artwork->isHidden() && !m_official_artwork.isNull()) {
			ShowArtwork(m_official_artwork, true);
		}
	});
}

void MainDialogPrivate::ChooseBenchmarkOutput() {
	const auto output = QFileDialog::getSaveFileName(
	    m_main_dialog, tr("Save 120-second benchmark"),
	    QDir::home().filePath(QStringLiteral("kyty-benchmark.json")), tr("JSON report (*.json)"),
	    nullptr, QFileDialog::DontConfirmOverwrite);
	if (output.isEmpty()) {
		return;
	}
	const auto path = output.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
	                      ? output
	                      : output + QStringLiteral(".json");
	if (QFileInfo::exists(path)) {
		QMessageBox::warning(m_main_dialog, tr("Benchmark report"),
		                     tr("Choose a new file name. Existing benchmark reports are preserved."));
		return;
	}
	Run({}, path);
}

void MainDialogPrivate::FindInterpreter() {
	UpdateChecker::ShowPreviousResult(m_main_dialog);
	QDir search_dir(QApplication::applicationDirPath());
	m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);

	if (!QFile::exists(m_interpreter)) {
		search_dir.cdUp();
		m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);
	}

	bool found = QFile::exists(m_interpreter);
	m_ui->widget->SetRuntimeDirectory(QFileInfo(m_interpreter).absolutePath());

	if (found) {
		m_ui->label_Interpreter->setText(tr("Emulator: ") + m_interpreter);

		QProcess test;
		test.setProgram(m_interpreter);
		test.start();
		test.waitForFinished();

		auto output = QString(test.readAllStandardOutput());
		auto lines  = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

		if (lines.count() >= 2) {
			m_ui->label_Version->setText(
			    tr("Version: ") + (lines.at(0).startsWith("exe_name") ? lines.at(1) : lines.at(0)));
			m_ui->label_Version->setToolTip(m_ui->label_Interpreter->text() + QStringLiteral("\n") +
			                               m_ui->label_settings_file->text());
		} else {
			found = false;
		}
	}

	if (!found) {
		QMessageBox::critical(m_main_dialog, tr("Error"), tr("Can't find emulator"));
		QApplication::quit();
		return;
	}

	m_ui->label_settings_file->setText(tr("Settings file: ") + m_ui->widget->GetSettingsFile());

	Update();
	if (m_ui->check_updates_on_startup->isChecked()) {
		m_update_checker->Check(false);
	}
}

static void AddFlag(QStringList& args, bool enabled, const char* name) {
	if (enabled) {
		args << name;
	}
}

static QStringList CreateEmulatorArgs(const Configuration& info) {
	QStringList args;
	auto        r = Configuration::NormalizeResolution(info.screen_resolution).split('x');

	if (r.size() != 2) {
		return {};
	}

	args << "--screen-width" << r.at(0);
	args << "--screen-height" << r.at(1);
	args << "--user-name" << info.user_name;
	args << "--user-id" << QString::number(info.user_id);
	if (!info.audio_input_device.isEmpty()) {
		args << "--mic" << info.audio_input_device;
	}
	if (!info.controller.color.isEmpty()) {
		args << "--controller-color" << info.controller.color;
	}
	args << "--controller-volume" << QString::number(info.controller.speaker_volume);
	args << "--controller-vibration" << QString::number(info.controller.vibration_intensity);
	args << "--keyboard-player" << QString::number(info.controller.keyboard_player);
	args << "--present-mode" << EnumToText(info.present_mode);
	if (info.gpu_index >= 0) {
		args << "--gpu" << QString::number(info.gpu_index);
	}
	AddFlag(args, info.fullscreen_enabled, "--fullscreen");
	AddFlag(args, info.hdr_enabled, "--hdr");
	AddFlag(args, info.hide_cursor_enabled, "--hide-cursor");
	AddFlag(args, info.stretch_enabled, "--stretch");
	AddFlag(args, info.readback_linear_images, "--readback-linear-images");
	AddFlag(args, info.readback_tiled_images, "--readback-tiled-images");
	AddFlag(args, info.readback_storage_images, "--readback-storage-images");
	AddFlag(args, info.sync_raw_image_buffers, "--sync-raw-image-buffers");
	args << "--flush-draws" << QString::number(info.flush_draws);
	AddFlag(args, !info.trophy_enabled, "--no-trophy-notifications");
	args << "--trophy-duration" << QString::number(info.trophy_duration_seconds);
	AddFlag(args, info.perf_overlay_enabled, "--perf-overlay");
	AddFlag(args, info.skip_notice_screen, "--skip-notice-screen");
	if (!info.launch_activity.isEmpty()) {
		args << "--launch-activity" << info.launch_activity;
	}
	AddFlag(args, info.network_connected, "--network-connected");
	AddFlag(args, info.psn_emulation_enabled, "--emulate-psn");
	AddFlag(args, info.tessellation_enabled, "--tessellation");
	AddFlag(args, info.occlusion_queries_enabled, "--occlusion-queries");
	AddFlag(args, info.relaxed_shader_float, "--relaxed-shader-float");
	AddFlag(args, info.native_rt, "--native-rt");
	AddFlag(args, info.shader_cache_enabled, "--shader-cache");
	if (info.shader_cache_enabled && info.pipeline_warmup_limit >= 0) {
		args << "--pipeline-warmup-limit" << QString::number(info.pipeline_warmup_limit);
	}
	AddFlag(args, info.archive_read_ahead, "--archive-read-ahead");
	args << "--vblank-frequency" << QString::number(info.vblank_frequency);
	args << "--console-language" << QString::number(info.console_language);
	AddFlag(args, info.vulkan_validation_enabled, "--vulkan-validation");
	AddFlag(args, info.shader_validation_enabled, "--shader-validation");
	args << "--shader-log-direction" << EnumToText(info.shader_log_direction);
	args << "--shader-log-folder" << info.shader_log_folder;
	AddFlag(args, info.command_buffer_dump_enabled, "--command-buffer-dump");
	args << "--command-buffer-dump-folder" << info.command_buffer_dump_folder;
	args << "--printf-direction" << EnumToText(info.printf_direction);
	args << "--printf-output-file" << info.printf_output_file;
	AddFlag(args, info.profiler_enabled, "--profile");
	AddFlag(args, info.pro_enabled, "--trinity");
	if (info.pro_enabled) {
		args << "--pssr-upscaler" << EnumToText(info.pssr_upscaler);
		if (info.pssr_upscaler == Configuration::PssrUpscaler::DLSS) {
			args << "--dlss-folder" << info.dlss_folder;
		} else if (info.pssr_upscaler == Configuration::PssrUpscaler::FSR) {
			args << "--fsr-folder" << info.fsr_folder;
		}
	}
	AddFlag(args, info.amd_cpu_enabled, "--amd-cpu");
	AddFlag(args, info.guest_tsc_enabled, "--guest-tsc");
#if defined(_WIN32)
	AddFlag(args, info.red_zone_protection_enabled, "--redzone");
#endif
	for (const auto& binding: info.host_input_mapping) {
		args << "--keymap" << binding;
	}
	AddFlag(args, info.renderdoc_enabled, "--rd");

	QString game = info.basedir;
	if (!info.elf.isEmpty() && !GameContent::IsArchive(info.basedir)) {
		game = QDir(info.basedir).filePath(info.elf);
	}
	args << "--game" << game;
	if (!info.addons_dir.isEmpty()) {
		args << "--addcont-dir" << info.addons_dir;
	}
	for (const auto& entry: info.addcont) {
		args << "--addcont" << entry;
	}

	const auto patch_plan = Cheats::PlanPath(info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << "--game-patch" << patch_plan;
	}

	return args;
}

#ifdef __linux__
// Runs the command in bash, then waits for a key (with close_on_clean_exit, only after an error).
// The command goes in as positional parameters ("$@"), so it needs no quoting.
static QStringList BashCommand(const QString& interpreter, const QStringList& args,
                               bool close_on_clean_exit) {
	const QString run = close_on_clean_exit ? "\"$@\" && exit" : "\"$@\"";
	return QStringList {"bash", "-c", run + "; echo Press any key...; read -n1", "kyty",
	                    interpreter} +
	       args;
}

// Find a terminal and its command separator.
static bool FindTerminal(QString* program, QStringList* prefix) {
	struct TerminalSpec {
		const char* executable;
		const char* separator; // nullptr when the command follows immediately
	};

	static const TerminalSpec candidates[] = {
	    {"x-terminal-emulator", "-e"},
	    {"gnome-terminal", "--"},
	    {"konsole", "-e"},
	    {"xfce4-terminal", "-x"},
	    {"mate-terminal", "--"},
	    {"tilix", "-e"},
	    {"alacritty", "-e"},
	    {"kitty", nullptr},
	    {"foot", nullptr},
	    {"wezterm", "-e"},
	    {"urxvt", "-e"},
	    {"xterm", "-e"},
	};

	const auto try_candidate = [program, prefix](const QString& executable, const char* separator) {
		const auto resolved = QStandardPaths::findExecutable(executable);
		if (resolved.isEmpty()) {
			return false;
		}
		*program = resolved;
		prefix->clear();
		if (separator != nullptr) {
			*prefix << QString::fromLatin1(separator);
		}
		return true;
	};

	if (const auto from_env = qEnvironmentVariable("TERMINAL"); !from_env.isEmpty()) {
		// Reuse the known separator for an explicit terminal.
		const auto  env_name  = QFileInfo(from_env).fileName();
		const char* separator = "-e";
		for (const auto& candidate: candidates) {
			if (env_name == QLatin1String(candidate.executable)) {
				separator = candidate.separator;
				break;
			}
		}
		if (try_candidate(from_env, separator)) {
			return true;
		}
	}

	for (const auto& candidate: candidates) {
		if (try_candidate(QString::fromLatin1(candidate.executable), candidate.separator)) {
			return true;
		}
	}

	return false;
}
#endif

#if defined(_WIN32)
// cmd /K arguments that keep the console open (with close_on_clean_exit, only after an error).
// A quote is written as "" rather than \" so cmd's quote count stays even and operators inside
// arguments stay quoted.
static QString CmdCommand(const QString& interpreter, const QStringList& args,
                          bool close_on_clean_exit) {
	static const QRegularExpression quote_or_end(QStringLiteral(R"((\\*)("|\z))"));
	// The UCRT reads 2n backslashes before a quote as n.
	auto quoted = QStringList {interpreter} + args;
	quoted.replaceInStrings(quote_or_end, QStringLiteral(R"(\1\1\2\2)"));
	return QStringLiteral("/K \"\"") + quoted.join(QStringLiteral("\" \"")) +
	       (close_on_clean_exit ? "\" && exit\"" : "\"\"");
}
#endif

void MainDialog::RunInterpreter(QProcess* process, const Configuration& info, bool close_terminal,
                                const QString& benchmark_output) {
	const auto& interpreter = m_p->GetInterpreter();

	auto args = CreateEmulatorArgs(info);
	if (args.isEmpty()) {
		QMessageBox::critical(this, tr("Error"), tr("Invalid emulator configuration"));
		QApplication::quit();
		return;
	}

	if (!benchmark_output.isEmpty()) {
		args << "--benchmark-output" << benchmark_output << "--benchmark-seconds" << "120";
	}

	// Without a terminal, the emulator runs directly.
	process->setProgram(interpreter);
	process->setArguments(args);
#ifdef __linux__
	QString     terminal;
	QStringList terminal_prefix;
	if (benchmark_output.isEmpty() && FindTerminal(&terminal, &terminal_prefix)) {
		process->setProgram(terminal);
		process->setArguments(terminal_prefix + BashCommand(interpreter, args, close_terminal));
	}
#elif defined(_WIN32)
	// Use nativeArguments so Qt does not re-quote the /K command string.
	if (benchmark_output.isEmpty()) {
		process->setProgram(CMD_EXE);
		process->setArguments({});
		process->setNativeArguments(
		    CmdCommand(QDir::toNativeSeparators(interpreter), args, close_terminal));
	}
#endif
	process->setWorkingDirectory(QFileInfo(interpreter).absolutePath());
#if defined(_WIN32)
	process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->flags |= static_cast<uint32_t>(CREATE_NEW_CONSOLE);
		args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USECOUNTCHARS);
		args->startupInfo->dwXCountChars = CMD_X_CHARS;
		args->startupInfo->dwYCountChars = CMD_Y_CHARS;
		// args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USEFILLATTRIBUTE);
		// args->startupInfo->dwFillAttribute =
		//     static_cast<DWORD>(BACKGROUND_BLUE) | static_cast<DWORD>(FOREGROUND_RED) |
		//     static_cast<DWORD>(FOREGROUND_INTENSITY);
	});
#endif
	process->start();
#if !defined(_WIN32)
	// Report immediate launch failures.
	if (!process->waitForStarted(5000)) {
		QMessageBox::critical(
		    this, tr("Error"),
		    tr("Failed to start:\n%1\n\n%2").arg(process->program(), process->errorString()));
		return;
	}
#endif
	process->waitForFinished(100);
}

void MainDialog::WriteSettings(QSettings& s) {
	MainDialogPrivate::WriteSettings(s);
}

void MainDialog::ReadSettings(QSettings& s) {
	MainDialogPrivate::ReadSettings(s);
}

void MainDialog::resizeEvent(QResizeEvent* event) {
	emit Resize();
	QDialog::resizeEvent(event);
}

void MainDialogPrivate::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry);
	}
	s.setValue(SETTINGS_CHECK_UPDATES, g_check_updates_on_startup);
	s.setValue(SETTINGS_CLOSE_TERMINAL, g_close_terminal);
	s.setValue(SETTINGS_THEME, g_theme);
	s.setValue(SETTINGS_ARTWORK, g_artwork_path);

	s.endGroup();
}

void MainDialogPrivate::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	g_last_geometry = s.value(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry).toByteArray();
	g_check_updates_on_startup = s.value(SETTINGS_CHECK_UPDATES, true).toBool();
	g_close_terminal           = s.value(SETTINGS_CLOSE_TERMINAL, false).toBool();
	g_theme                    = s.value(SETTINGS_THEME, 3).toInt();
	g_artwork_path             = s.value(SETTINGS_ARTWORK).toString();
	if (g_theme < 0 || g_theme > 3) {
		g_theme = 3;
	}

	s.endGroup();
}

void MainDialogPrivate::Run(const QString& activity, const QString& benchmark_output) {
	if (!m_launch->isEnabled() || m_update_checker->IsInstalling() ||
	    (!benchmark_output.isEmpty() && m_running_games > 0)) {
		return;
	}
	QPointer<ConfigurationItem> item = m_ui->widget->GetSelectedItem();
	if (item == nullptr) {
		return;
	}

	item->SetRunning(true);
	m_running_games++;
	m_controller_preview.SetSuspended(true);
	m_title_sound.SetTitle({});

	auto* process = new QProcess(this);
	if (!benchmark_output.isEmpty()) {
		process->setProcessChannelMode(QProcess::ForwardedChannels);
		connect(process, &QProcess::finished, m_main_dialog,
		        [dialog = m_main_dialog, benchmark_output](int code, QProcess::ExitStatus status) {
			        if (code == 0 && status == QProcess::NormalExit && QFileInfo::exists(benchmark_output)) {
				        QMessageBox::information(dialog, tr("Benchmark saved"), benchmark_output);
			        } else {
				        QMessageBox::warning(dialog, tr("Benchmark incomplete"),
				                             tr("The emulator exited before saving the benchmark report."));
			        }
		        });
	}
	connect(process, &QProcess::stateChanged, this,
	        [this, process, item](QProcess::ProcessState state) {
		        if (state == QProcess::NotRunning) {
			        if (item != nullptr) {
				        item->SetRunning(false);
			        }
			        m_running_games--;
			        process->deleteLater();
			        Update();
		        }
	        });
	auto info             = m_ui->widget->CreateConfiguration(*item);
	info->launch_activity = activity;
	m_main_dialog->RunInterpreter(process, *info, g_close_terminal, benchmark_output);

	Update();
}

void MainDialogPrivate::Update() {
	const auto* item = m_ui->widget->GetSelectedItem();

	bool run_enabled = !m_interpreter.isEmpty() && item != nullptr && !item->IsRunning() &&
	                   !m_update_checker->IsInstalling();

	if (run_enabled) {
		const auto& info = item->GetInfo();
		auto        dir  = info.basedir;
		run_enabled      = !dir.isEmpty() && (QDir(dir).exists() || GameContent::IsArchive(dir));
	}

	m_ui->widget->SetRunEnabled(run_enabled);
	m_launch->setEnabled(run_enabled);
	m_benchmark->setEnabled(run_enabled && m_running_games == 0);
	m_settings->setEnabled(!m_update_checker->IsInstalling() &&
	                       (item == nullptr || !item->IsRunning()));
	m_settings->setText(item == nullptr ? tr("Settings") : tr("Game settings"));
	m_launch->setText(item != nullptr && item->GetInfo().IsGtaVi() ? tr("Launch GTA VI")
	                                                             : tr("Launch selected"));
	if (item == nullptr) {
		m_selection->setText(tr("Add a game folder, then select a title in the library."));
		m_selection->setToolTip({});
	} else {
		const auto& info = item->GetInfo();
		auto title = info.name.isEmpty() ? QFileInfo(info.basedir).fileName() : info.name;
		const auto effective = m_ui->widget->CreateConfiguration(*item);
		m_selection->setText(item->IsRunning()
		                         ? tr("%1 is running").arg(title)
		                         : tr("%1  ·  %2  ·  %3")
		                               .arg(title, effective->screen_resolution,
		                                    EnumToText(effective->present_mode)));
		m_selection->setToolTip(info.basedir);
	}
	UpdateArtwork(item);
	m_update_checker->SetGameRunning(m_running_games > 0);
	if (m_running_games > 0) {
		m_controller_preview.SetSuspended(true);
		return;
	}
	m_controller_preview.SetSuspended(false);
	m_controller_preview.SetColor(m_ui->widget->GetGlobalControllerColor());
}

#include "mainDialog.moc"

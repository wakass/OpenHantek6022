// SPDX-License-Identifier: GPL-2.0+

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDesktopWidget>
#include <QElapsedTimer>
#include <QLibraryInfo>
#include <QLocale>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QTranslator>
#ifdef Q_OS_LINUX
#include <sched.h>
#endif
#include <iostream>
#ifdef Q_OS_FREEBSD
#include <libusb.h>
// FreeBSD doesn't have libusb_setlocale()
#define libusb_setlocale( x ) (void)0
#else
#include <libusb-1.0/libusb.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <math.h>
#include <memory>

// Settings
#include "dsosettings.h"
#include "viewconstants.h"
#include "viewsettings.h"

// DSO core logic
#include "capturing.h"
#include "dsomodel.h"
#include "hantekdsocontrol.h"
#include "usb/scopedevice.h"

// Post processing
#include "post/graphgenerator.h"
#include "post/mathchannelgenerator.h"
#include "post/postprocessing.h"
#include "post/spectrumgenerator.h"

// Exporter
#include "exporting/exportcsv.h"
#include "exporting/exporterprocessor.h"
#include "exporting/exporterregistry.h"
#include "exporting/exportjson.h"

// GUI
#include "iconfont/QtAwesome.h"
#include "mainwindow.h"
#include "selectdevice/selectsupporteddevice.h"

// OpenGL setup
#include "glscope.h"

#include "models/modelDEMO.h"

#ifndef VERSION
#error "You need to run the cmake buildsystem!"
#endif
#include "OH_VERSION.h"


using namespace Hantek;


/// \brief Initialize resources and translations and show the main window.
int main( int argc, char *argv[] ) {

#ifdef Q_OS_WIN
    // Win: close "extra" console window but if started from cmd.exe use this console
    if ( FreeConsole() && AttachConsole( ATTACH_PARENT_PROCESS ) ) {
        freopen( "CONOUT$", "w", stdout );
        freopen( "CONOUT$", "w", stderr );
    }
#else
    // this ENV variable hides the LANG=xx setting, fkt. not available under Windows
    unsetenv( "LANGUAGE" );
#endif

    QElapsedTimer startupTime;
    startupTime.start(); // time tracking for verbose startup

    //////// Set application information ////////
    QCoreApplication::setOrganizationName( "OpenHantek" );
    QCoreApplication::setOrganizationDomain( "openhantek.org" );
    QCoreApplication::setApplicationName( "OpenHantek6022" );
    QCoreApplication::setApplicationVersion( VERSION );
    QCoreApplication::setAttribute( Qt::AA_UseHighDpiPixmaps, true );
#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 6, 0 ) )
    QCoreApplication::setAttribute( Qt::AA_EnableHighDpiScaling, true );
#endif

    qDebug() << ( QString( "%1 (%2)" ).arg( QCoreApplication::applicationName(), QCoreApplication::applicationVersion() ) )
                    .toLocal8Bit()
                    .data();

    bool demoMode = false;
    bool useGLES = false;
    bool useGLSL120 = false;
    bool useGLSL150 = false;
    bool useLocale = true;
    // verboseLevel allows the fine granulated tracing of the program for easy testing and debugging
    unsigned verboseLevel = 0; // 0: quiet; 1,2: startup; 3,4: + user actions; 5,6: + data processing
    bool resetSettings = false;
    QString font = defaultFont;       // defined in viewsettings.h
    int fontSize = defaultFontSize;   // defined in viewsettings.h
    int condensed = defaultCondensed; // defined in viewsettings.h

    { // do this early at program start ...
        // get font size settings:
        // Linux, Unix: $HOME/.config/OpenHantek/OpenHantek6022.conf
        // macOS:       $HOME/Library/Preferences/org.openhantek.OpenHantek6022.plist
        // Windows:     HKEY_CURRENT_USER\Software\OpenHantek\OpenHantek6022"
        // more info:   https://doc.qt.io/qt-5/qsettings.html#platform-specific-notes
        QSettings storeSettings;
        storeSettings.beginGroup( "view" );
        if ( storeSettings.contains( "fontSize" ) )
            fontSize = Dso::InterpolationMode( storeSettings.value( "fontSize" ).toInt() );
        storeSettings.endGroup();
    } // ... and forget the no more needed variables
    { // also here ...
        QCoreApplication parserApp( argc, argv );
        QCommandLineParser p;
        p.addHelpOption();
        p.addVersionOption();
        QCommandLineOption demoModeOption( {"d", "demoMode"}, "Demo mode without scope HW" );
        p.addOption( demoModeOption );
        QCommandLineOption useGlesOption( {"e", "useGLES"}, "Use OpenGL ES instead of OpenGL" );
        p.addOption( useGlesOption );
        QCommandLineOption useGLSL120Option( "useGLSL120", "Force OpenGL SL version 1.20" );
        p.addOption( useGLSL120Option );
        QCommandLineOption useGLSL150Option( "useGLSL150", "Force OpenGL SL version 1.50" );
        p.addOption( useGLSL150Option );
        QCommandLineOption intOption( {"i", "international"}, "Show the international interface, do not translate" );
        p.addOption( intOption );
        QCommandLineOption fontOption( {"f", "font"}, "Define the system font", "Font" );
        p.addOption( fontOption );
        QCommandLineOption sizeOption(
            {"s", "size"}, QString( "Set the font size (default = %1, 0: automatic from dpi)" ).arg( fontSize ), "Size" );
        p.addOption( sizeOption );
        QCommandLineOption condensedOption(
            {"c", "condensed"}, QString( "Set the font condensed value (default = %1)" ).arg( condensed ), "Condensed" );
        p.addOption( condensedOption );
        QCommandLineOption resetSettingsOption( "resetSettings", "Reset persistent settings, start with default" );
        p.addOption( resetSettingsOption );
        QCommandLineOption verboseOption( "verbose", "Verbose tracing of program startup, ui and processing steps", "Level" );
        p.addOption( verboseOption );
        p.process( parserApp );
        demoMode = p.isSet( demoModeOption );
        if ( p.isSet( fontOption ) )
            font = p.value( "font" );
        if ( p.isSet( sizeOption ) )
            fontSize = p.value( "size" ).toInt();
        if ( p.isSet( condensedOption ) ) // allow range from UltraCondensed (50) to UltraExpanded (200)
            condensed = qBound( 50, p.value( "condensed" ).toInt(), 200 );
        useGLES = p.isSet( useGlesOption );
        useGLSL120 = p.isSet( useGLSL120Option );
        useGLSL150 = p.isSet( useGLSL150Option );
        useLocale = !p.isSet( intOption );
        if ( p.isSet( verboseOption ) )
            verboseLevel = p.value( "verbose" ).toUInt();
        resetSettings = p.isSet( resetSettingsOption );
    } // ... and forget the no more needed variables


    if ( verboseLevel ) {
        qDebug() << startupTime.elapsed() << "ms:"
                 << "OpenHantek6022 - version" << VERSION;
        qDebug() << startupTime.elapsed() << "ms:"
                 << "create openHantekApplication";
    }
    QApplication openHantekApplication( argc, argv );

// Qt5 linux default ("Breeze", "Windows" or "Fusion")
#ifndef Q_OS_MACOS
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "set \"Fusion\" style";
    openHantekApplication.setStyle( QStyleFactory::create( "Fusion" ) ); // smaller widgets allow stacking of all docks
#endif

#ifdef Q_OS_LINUX
    // try to set realtime priority to improve USB allocation
    // this works if the user is member of a realtime group, e.g. audio:
    // 1. set limits in /etc/security/limits.d:
    //    @audio - rtprio 99
    // 2. add user to the group, e.g. audio:
    //    usermod -a -G audio <your_user_name>
    // or set the limits only for your user in /etc/security/limits.d:
    //    <your_user_name> - rtprio 99
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "set RT FIFO scheduler";
    struct sched_param schedParam;
    schedParam.sched_priority = 9;                    // set RT priority level 10
    sched_setscheduler( 0, SCHED_FIFO, &schedParam ); // and RT FIFO scheduler
    // but ignore any error if user has no realtime rights
#endif

    //////// Load translations ////////
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "load translations for locale" << QLocale().name();
    QTranslator qtTranslator;
    QTranslator openHantekTranslator;
    if ( useLocale && QLocale().name() != "en_US" ) { // somehow Qt on MacOS uses the german translation for en_US?!
        if ( qtTranslator.load( "qt_" + QLocale().name(), QLibraryInfo::location( QLibraryInfo::TranslationsPath ) ) ) {
            openHantekApplication.installTranslator( &qtTranslator );
        }
        if ( openHantekTranslator.load( QLocale(), QLatin1String( "openhantek" ), QLatin1String( "_" ),
                                        QLatin1String( ":/translations" ) ) ) {
            openHantekApplication.installTranslator( &openHantekTranslator );
        }
    }

    //////// Find matching usb devices - show splash screen ////////
    libusb_context *context = nullptr;

    std::unique_ptr< ScopeDevice > scopeDevice = nullptr;

    if ( !demoMode ) {
        if ( verboseLevel )
            qDebug() << startupTime.elapsed() << "ms:"
                     << "init libusb";
        int error = libusb_init( &context );
        if ( error ) {
            SelectSupportedDevice().showLibUSBFailedDialogModel( error );
            return -1;
        }
        if ( useLocale ) // localize USB error messages, supported: "en", "nl", "fr", "ru"
            libusb_setlocale( QLocale().name().toLocal8Bit().constData() );

        // SelectSupportedDevive returns a real device unless demoMode is true
        if ( verboseLevel )
            qDebug() << startupTime.elapsed() << "ms:"
                     << "show splash screen";
        scopeDevice = SelectSupportedDevice().showSelectDeviceModal( context, verboseLevel );
        if ( scopeDevice && scopeDevice->isDemoDevice() ) {
            demoMode = true;
            libusb_exit( context ); // stop all USB activities
            context = nullptr;
        } else {
            QString errorMessage;
            if ( scopeDevice == nullptr || !scopeDevice->connectDevice( errorMessage ) ) {
                libusb_exit( context ); // clean USB
                if ( !errorMessage.isEmpty() )
                    qCritical() << errorMessage;
                return -1;
            }
        }
    } else {
        scopeDevice = std::unique_ptr< ScopeDevice >( new ScopeDevice() );
    }

    // Here we have either a connected scope device or a demo device w/o hardware
    const DSOModel *model = scopeDevice->getModel();
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "use device" << scopeDevice->getModel()->name << "serial number" << scopeDevice->getSerialNumber();

    //////// Create DSO control object and move it to a separate thread ////////
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "create DSO control thread";
    QThread dsoControlThread;
    dsoControlThread.setObjectName( "dsoControlThread" );
    HantekDsoControl dsoControl( scopeDevice.get(), model, verboseLevel );
    dsoControl.moveToThread( &dsoControlThread );
    QObject::connect( &dsoControlThread, &QThread::started, &dsoControl, &HantekDsoControl::stateMachine );
    QObject::connect( &dsoControl, &HantekDsoControl::communicationError, QCoreApplication::instance(), &QCoreApplication::quit );

    if ( scopeDevice )
        QObject::connect( scopeDevice.get(), &ScopeDevice::deviceDisconnected, QCoreApplication::instance(),
                          &QCoreApplication::quit );

    const Dso::ControlSpecification *spec = model->spec();

    //////// Create settings object specific to this scope, use unique serial number ////////
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "create settings object";
    DsoSettings settings( scopeDevice.get(), resetSettings );
    settings.scope.verboseLevel = verboseLevel;

    //////// Create exporters ////////
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "create exporters";
    ExporterRegistry exportRegistry( spec, &settings );
    ExporterCSV exporterCSV;
    ExporterJSON exporterJSON;
    ExporterProcessor samplesToExportRaw( &exportRegistry );
    exportRegistry.registerExporter( &exporterCSV );
    exportRegistry.registerExporter( &exporterJSON );

    //////// Create post processing objects ////////
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "create post processing objects";
    QThread postProcessingThread;
    postProcessingThread.setObjectName( "postProcessingThread" );
    PostProcessing postProcessing( settings.scope.countChannels(), verboseLevel );

    SpectrumGenerator spectrumGenerator( &settings.scope, &settings.post );
    MathChannelGenerator mathchannelGenerator( &settings.scope, spec->channels );
    GraphGenerator graphGenerator( &settings.scope, &settings.view );

    postProcessing.registerProcessor( &samplesToExportRaw );
    postProcessing.registerProcessor( &mathchannelGenerator );
    postProcessing.registerProcessor( &spectrumGenerator );
    postProcessing.registerProcessor( &graphGenerator );

    postProcessing.moveToThread( &postProcessingThread );
    QObject::connect( &dsoControl, &HantekDsoControl::samplesAvailable, &postProcessing, &PostProcessing::input );
    QObject::connect( &postProcessing, &PostProcessing::processingFinished, &exportRegistry, &ExporterRegistry::input,
                      Qt::DirectConnection );

    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "setup OpenGL";

    // set appropriate OpenGLSL version
    // some not so new intel graphic driver report a very conservative version
    // even if they deliver OpenGL 4.x functions
    // e.g. debian buster -> "2.1 Mesa 18.3.6"
    // Standard W10 installation -> "OpenGL ES 2.0 (ANGLE 2.1.0.57ea533f79a7)"
    // MacOS supports OpenGL 4.4 since 2011, 3.3 before

    // This is the default setting for Mesa (Linux, FreeBSD)
    QString GLSLversion = GLSL120;

#if defined( Q_OS_MAC )
    // recent MacOS uses OpenGL 4.4, but at least 3.3 for very old systems (<2011)
    GLSLversion = GLSL150;
#elif defined( Q_PROCESSOR_ARM )
    // Raspberry Pi crashes with OpenGL, use OpenGLES
    GLSLversion = GLES100;
#endif

    // some fresh W10 installations announce this
    // "OpenGL ES 2.0 (ANGLE ...)"
    if ( QRegularExpression( "OpenGL ES " ).match( GlScope::getOpenGLversion() ).hasMatch() )
        GLSLversion = GLES100; // set as default

    // override default with command line option
    if ( useGLES ) // 1st priority
        GLSLversion = GLES100;
    else if ( useGLSL120 ) // next
        GLSLversion = GLSL120;
    else if ( useGLSL150 ) // least prio
        GLSLversion = GLSL150;

    GlScope::useOpenGLSLversion( GLSLversion ); // prepare the OpenGL renderer

    //////// Prepare visual appearance ////////
    // prepare the font size and style settings for the scope application
    QFont appFont = openHantekApplication.font();
    if ( 0 == fontSize ) {                               // option -s0 -> use system font size
        fontSize = qBound( 6, appFont.pointSize(), 24 ); // values < 6 do not scale correctly
    }
    // remember the actual fontsize setting
    settings.view.fontSize = fontSize;
    appFont.setFamily( font ); // Fusion style + Arial (default) -> fit on small screen (Y >= 720 pixel)
    appFont.setStretch( condensed );
    appFont.setPointSize( fontSize ); // scales the widgets accordingly
    // apply new font settings for the scope application
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "set" << appFont;
    openHantekApplication.setFont( appFont );
    openHantekApplication.setFont( appFont, "QWidget" ); // on some systems the 2nd argument is required

    iconFont->initFontAwesome();

    //////// Create main window ////////
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "create main window";
    MainWindow openHantekMainWindow( &dsoControl, &settings, &exportRegistry );
    QObject::connect( &postProcessing, &PostProcessing::processingFinished, &openHantekMainWindow, &MainWindow::showNewData );
    QObject::connect( &exportRegistry, &ExporterRegistry::exporterProgressChanged, &openHantekMainWindow,
                      &MainWindow::exporterProgressChanged );
    QObject::connect( &exportRegistry, &ExporterRegistry::exporterStatusChanged, &openHantekMainWindow,
                      &MainWindow::exporterStatusChanged );
    openHantekMainWindow.show();

    //////// Start DSO thread and go into GUI main loop
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "start DSO control thread";
    dsoControl.enableSampling( true );
    postProcessingThread.start();
    dsoControlThread.start();
    Capturing capturing( &dsoControl );
    capturing.start();

    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "execute GUI main loop";
    int appStatus = openHantekApplication.exec();

    //////// Application closed, clean up step by step ////////
    if ( verboseLevel )
        qDebug() << startupTime.elapsed() << "ms:"
                 << "application closed, clean up";

    std::cout << std::unitbuf; // enable automatic flushing

    // the stepwise text output gives some hints about the shutdown timing
    // not needed with appropriate verbose level
    if ( verboseLevel < 3 )
        std::cout << "OpenHantek6022 "; // 1st part

    dsoControl.quitSampling(); // send USB control command, stop bulk transfer

    // stop the capturing thread
    // wait 2 * record time (delay is ms) for dso to finish
    unsigned waitForDso = unsigned( 2000 * dsoControl.getSamplesize() / dsoControl.getSamplerate() );
    waitForDso = qMax( waitForDso, 10000U ); // wait for at least 10 s
    capturing.requestInterruption();
    capturing.wait( waitForDso );
    if ( verboseLevel < 3 )
        std::cout << "has "; // 2nd part

    // now quit the data acquisition thread
    // wait 2 * record time (delay is ms) for dso to finish
    dsoControlThread.quit();
    dsoControlThread.wait( waitForDso );
    if ( verboseLevel < 3 )
        std::cout << "stopped "; // 3rd part

    // next stop the data processing
    postProcessing.stop();
    postProcessingThread.quit();
    postProcessingThread.wait( 10000 );
    if ( verboseLevel < 3 )
        std::cout << "after "; // 4th part

    // finally shut down the libUSB communication
    if ( scopeDevice )
        scopeDevice.reset(); // destroys unique_pointer, causes libusb_close(), must be called before libusb_exit()
    if ( context )
        libusb_exit( context );

    if ( verboseLevel < 3 )
        std::cout << openHantekMainWindow.elapsedTime.elapsed() / 1000 << " s\n"; // last part
    else
        std::cout << "OpenHantek6022 has stopped after " << openHantekMainWindow.elapsedTime.elapsed() / 1000 << " s\n";

    return appStatus;
}

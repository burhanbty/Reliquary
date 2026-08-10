#include "interface_language.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QHash>
#include <QTranslator>
#include <QXmlStreamReader>

TEST(UiLanguage, FirstRunUsesTurkishSystemLocale) {
    EXPECT_EQ(vidstorex_ui::resolve_language({}, "tr_TR"), "tr");
    EXPECT_EQ(vidstorex_ui::resolve_language({}, "tr-CY"), "tr");
}

TEST(UiLanguage, FirstRunUsesEnglishForOtherLocales) {
    EXPECT_EQ(vidstorex_ui::resolve_language({}, "en_US"), "en");
    EXPECT_EQ(vidstorex_ui::resolve_language({}, "de_DE"), "en");
}

TEST(UiLanguage, SavedPreferenceOverridesSystemLocale) {
    EXPECT_EQ(vidstorex_ui::resolve_language("en", "tr_TR"), "en");
    EXPECT_EQ(vidstorex_ui::resolve_language("tr", "en_US"), "tr");
}

TEST(UiLanguage, UnknownSavedPreferenceFallsBackSafely) {
    EXPECT_EQ(vidstorex_ui::resolve_language("unknown", "tr_TR"), "en");
}

TEST(UiLanguage, LanguageCodesAreStable) {
    EXPECT_STREQ(vidstorex_ui::kEnglishLanguage, "en");
    EXPECT_STREQ(vidstorex_ui::kTurkishLanguage, "tr");
    EXPECT_TRUE(vidstorex_ui::is_supported_language("en"));
    EXPECT_TRUE(vidstorex_ui::is_supported_language("tr"));
    EXPECT_FALSE(vidstorex_ui::is_supported_language("TR"));
}

TEST(UiLanguage, TurkishCatalogCoversCriticalUserInterface) {
    QFile file(QStringLiteral(VIDSTOREX_SOURCE_DIR)
        + "/translations/vidstorex_tr.ts");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QXmlStreamReader xml(&file);
    QHash<QString, QString> messages;
    QString source;
    QString translation;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("message")) {
            source.clear();
            translation.clear();
        } else if (xml.isStartElement() &&
                   xml.name() == QLatin1String("source")) {
            source = xml.readElementText();
        } else if (xml.isStartElement() &&
                   xml.name() == QLatin1String("translation")) {
            translation = xml.readElementText();
            if (!source.isEmpty()) messages.insert(source, translation);
        }
    }
    ASSERT_FALSE(xml.hasError()) << xml.errorString().toStdString();
    EXPECT_GE(messages.size(), 150);
    const QHash<QString, QString> critical{
        {"Turn files into resilient videos and recover them later.",
         QString::fromUtf8("Dosyalarınızı dayanıklı videolara dönüştürün ve daha sonra geri kurtarın.")},
        {"Create a Video Set", QString::fromUtf8("Video Set Oluştur")},
        {"Recover a File", QString::fromUtf8("Dosyayı Kurtar")},
        {"Most Reliable", QString::fromUtf8("En Güvenli")},
        {"Fewer & Shorter Videos", QString::fromUtf8("Daha Az ve Daha Kısa Video")},
        {"Everything is ready.", QString::fromUtf8("Her şey hazır.")},
        {"Your file was recovered exactly.",
         QString::fromUtf8("Dosyanız birebir kurtarıldı.")},
        {"Store your files safely in videos",
         QString::fromUtf8("Dosyalarınızı videolarda güvenle saklayın")},
        {"Advanced / Classic Video Set Tools",
         QString::fromUtf8("Gelişmiş / Klasik Video Set Araçları")},
        {"Technical tools, experiments and low-level controls.",
         QString::fromUtf8("Teknik araçlar, deneyler ve düşük seviyeli kontroller.")},
        {"No recent Video Sets yet.",
         QString::fromUtf8("Henüz Video Set oluşturmadınız.")},
        {"Technical details", QString::fromUtf8("Teknik ayrıntılar")},
        {"Copy manifest location",
         QString::fromUtf8("Manifest konumunu kopyala")},
        {"Download Processed Videos",
         QString::fromUtf8("İşlenmiş Videoları İndir")},
        {"Experimental", QString::fromUtf8("Deneysel")},
        {"YouTube Sync (Experimental)",
         QString::fromUtf8("YouTube Senkronizasyonu (Deneysel)")},
        {"Upload all parts to YouTube.",
         QString::fromUtf8("Tüm parçaları YouTube'a yükleyin.")},
        {"Open YouTube", QString::fromUtf8("YouTube'u Aç")},
        {"Turn your file into videos",
         QString::fromUtf8("Dosyanızı videolara dönüştürün")},
        {"Store the videos", QString::fromUtf8("Videoları saklayın")},
        {"Paste the playlist. Get your file back.",
         QString::fromUtf8("Playlist bağlantısını yapıştırın, dosyanızı geri alın")},
        {"Start Using VidStoreX",
         QString::fromUtf8("VidStoreX'i Kullanmaya Başla")},
        {"Show Getting Started Again",
         QString::fromUtf8("Başlangıç Rehberini Tekrar Göster")}};
    for (auto it = critical.cbegin(); it != critical.cend(); ++it) {
        ASSERT_TRUE(messages.contains(it.key()))
            << it.key().toStdString();
        EXPECT_EQ(messages.value(it.key()), it.value());
    }
    for (auto it = messages.cbegin(); it != messages.cend(); ++it)
        EXPECT_FALSE(it.value().trimmed().isEmpty())
            << it.key().toStdString();
}

TEST(UiLanguage, MissingCatalogFallsBackWithoutFailure) {
    QTranslator translator;
    EXPECT_FALSE(translator.load(":/i18n/does-not-exist.qm"));
}

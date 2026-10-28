# Video Set resume dosya yolu ve overwrite düzeltmesi

Tarih: 27 Ekim 2026. Kapsam: Development Task 01.

## Kök neden

`read_manifest()`, `expected_video_filename` alanını yol bileşeni olarak doğrulamadan okuyordu. `do_encode()` bu metni `staging/videos` yoluna ekliyor, ardından `ms_encode()` mevcut hedefin üzerine yazabilen API yolunu çağırıyordu. Manifest descriptor hash'i video dosya adını kapsamıyordu; dolayısıyla yalnız bu alanı değiştirmek descriptor kontrolünü bozmuyordu. Parça hedefinde ayrıca `--overwrite` kontrolü yoktu. Çıktı ve staging klasörlerinde symlink, junction veya hardlink denetimi de yapılmıyordu.

Kalıcı regresyon testi, düzeltmeden önce mevcut 1.5.0 ikilisiyle çalıştırıldı. `../../outside-video.mkv` ile, `--overwrite` kullanılmadan sentinelin SHA-256 değeri değişti ve test beklenen şekilde başarısız oldu:

```text
Önce: 2ecbb41e4529f8f6d1bf6376dbee412186f5005e1e03cf6b908e4ce909bbeac4
Sonra: be4d807f288caf42d0a4b9297bc3c090dfa0c9da009d6972c86226fb4ad8efab
```

Bu hedef gerçek kullanıcı dosyası değildi; testin kendi klasöründe, staging dışına yerleştirilmiş sentetik bir dosyaydı. Kayıt: `build-resume-security/red-regression.log`.

## Uygulanan değişiklikler

| Dosya | Değişiklik |
|---|---|
| `src/video_set.cpp` | Manifest okurken güvenli tek dosya adı kontrolü; yol ayraçları, mutlak/sürücü/UNC yolları, ADS, cihaz adları, kontrol karakterleri ve sonda nokta/boşluk gibi adları reddetme. Mevcut deterministik adlandırmayı yeniden hesaplayarak manifestteki adlarla karşılaştırma. |
| `src/video_set_cli.cpp` | Normalize edilmiş çıktı yolunun üst bileşenlerini ve staging ağacını gerçek dosya sistemi girdileri üzerinden denetleme. Windows reparse point, symlink, hardlink ve özel dosyaları reddetme. Tüm parçalarda ilk durum yazımından önce overwrite ön kontrolü; yazımlar öncesinde yeniden denetim. |
| `src/video_set_cli.cpp` | Tamamlanmış parçayı mevcut boyut ve SHA-256 kontrolleriyle atlama. Diğer mevcut videolar için açık `--overwrite` zorunluluğu. Encode işlemini kısa ve benzersiz bir geçici video yoluna yönlendirme; asıl hedefe ayrı, overwrite politikasını uygulayan yayınlama adımı. |
| `tests/test_video_set.cpp` | 18 tehlikeli veya deterministik adlandırmaya uymayan değer için manifest reddi; sentinel ve manifest SHA-256 değişmezliği kontrolü. |
| `tests/test_video_set_resume_security.cmake` | 22 kalıcı CLI senaryosu; izole sentetik staging, dosyalar ve bağlantılar; olumsuz senaryolarda sentinel SHA-256, manifest SHA-256 ve staging dosya listesi kontrolü. |
| `tests/CMakeLists.txt` | Yeni senaryoların CTest'e kaydı. |

Geçersiz manifest ve mevcut, doğrulanmamış video hedefi; manifest/backup/durum dosyaları yazılmadan önce reddediliyor. `--overwrite`, staging dışına veya bir bağlantı üzerinden yazma izni sağlamıyor. Normal resume için gerekli manifest ve durum güncellemeleri korunuyor.

## Test sonuçları

Derleme: MSVC 19.44, Release, GUI ve testler açık; ayrı `build-resume-security/compiled` klasörü. Kurulu vcpkg bağımlılıkları ve önbellekteki GoogleTest kullanıldı. Son derleme başarılı.

| Doğrulama | Sonuç |
|---|---|
| Düzeltme öncesi traversal regresyonu | Beklenen FAIL; sentinel değişimi doğrulandı. |
| Yeni manifest birim testi | PASS; 18 geçersiz değer reddedildi. |
| Yeni CLI regresyonları | 22/22 PASS. |
| İlgili tüm Video Set testleri | 67/67 PASS; CLI ve GUI uçtan uca testleri dahil. |
| Tam CTest paketi | 525/525 PASS; başarısız veya atlanan test yok, 131,51 saniye. |

CLI senaryoları: normal resume, aynı parolayla şifreli resume, tamamlanmış parça atlama, dört parçalı sette eksik parçayı yeniden üretme; traversal (`--overwrite` ile ve olmadan), mutlak yol, köklü yol, sürücüye göreli yol, UNC, ADS, alt klasör, ters eğik çizgi, Windows cihaz adı, sonda nokta; mevcut hedef, hash'i eşleşmeyen tamamlanmış hedef, açık izinli kapsam içi overwrite; manifest backup hardlink'i ve `videos`, staging, `reports` junction'ları.

Olumlu senaryolarda set tekrar kurtarıldı ve tam dosya SHA-256 değeri kaynakla eşleşti. Tamamlanmış parçanın yeniden encode edilmediği, video SHA-256 değişmezliği ve `already complete` çıktısıyla kontrol edildi. Her olumsuz CLI senaryosunda sentinel SHA-256 değişmedi. Bağlantı oluşturma başarısızlığı bir güvenlik testi başarısı veya atlama olarak sayılmadı.

İlk sandbox denemeleri junction/hardlink oluşturmayı ve bazı atomik dosya işlemlerini erişim hatasıyla engelledi. Bu denemeler PASS sayılmadı; aynı testler sandbox dışında yeniden çalıştırıldı. İlk geçici video yaklaşımı Windows yol uzunluğu sınırını aşabildi; kısa geçici adla giderildi ve olumlu testler tekrar geçti. Test beklentileri gevşetilmedi.

Tekrar çalıştırma:

```powershell
ctest --test-dir build-resume-security/compiled -R VideoSet --output-on-failure
ctest --test-dir build-resume-security/compiled --output-on-failure
```

Kanıtlar: `build-resume-security/build-final.log`, `video-set-results.xml`, `video-set-tests.log`, `full-results.xml`, `full-tests.log`. Test verileri ve derleme çıktıları Git tarafından yok sayılan çalışma klasöründedir. Gerçek YouTube hesabı, OAuth token veya kişisel arşiv kullanılmadı; GUI indirme testi sahte yt-dlp kullanır.

## Geriye dönük uyumluluk

Manifest sürümü, `VSXSET01` zarfı, parça sırası/kimlik algoritması, SHA-256, codec ve şifreleme biçimi değişmedi. Mevcut üretim kodunun deterministik adlarıyla oluşturulmuş manifestler ve bilinmeyen opsiyonel alanlar okunmaya devam ediyor. Şifreli ve şifresiz kurtarma ile GUI entegrasyonu test edildi.

Davranışın bilinçli daraltıldığı durumlar: elle değiştirilmiş, deterministik adla uyuşmayan video adları reddedilir. Encode hedefi/staging ağacındaki symlink, junction, diğer Windows reparse point ve hardlink'ler, kapsam içinde görünseler bile reddedilir. Hash'i eşleşmeyen mevcut bir video otomatik olarak değiştirilmez; yeniden üretmek için `--overwrite` gerekir.

## Kalan riskler

**TOCTOU yarışı tamamen giderilmedi.** Mevcut codec ve iostream API'leri dosyaları yol metniyle açıyor. Aynı anda staging veya üst dizinlerine yazabilen başka bir yerel süreç, denetim ile açma/yeniden adlandırma arasındaki aralıkta bir dizini veya bağlantıyı değiştirebilir. Yeniden denetimler pencereyi daraltır; handle tabanlı bir güvenlik sınırı oluşturmaz. Rastgele geçici ad ayırması da exclusive-create işlemi değildir. Bu çalışmada eşzamanlı saldırgan süreçlere karşı yarışsızlık iddiası veya testi yoktur. Tam giderim, no-follow/exclusive-create ve sabit dizin handle'larına dayanan daha kapsamlı dosya erişimi değişikliği gerektirir.

Windows reparse point kontrolü, [Microsoft'un reparse point işlemleri belgesindeki](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-point-operations) `FILE_ATTRIBUTE_REPARSE_POINT` davranışını izler. Handle ve paylaşım seçenekleri [CreateFile belgesinde](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) açıklanır; mevcut yol tabanlı çağrılar bu handle korumasını kullanmaz. Yarış sınırı, bu kaynaklar ve mevcut çağrı zincirinden yapılan değerlendirmedir.

Dinamik doğrulama Windows x64 üzerinde yapıldı; Linux/macOS üzerinde çalıştırılmadı.

## Git diff özeti ve son durum

Bu göreve ait toplam 6 dosya: **430 satır eklendi, 16 satır silindi**. Üretim kodu ve testler 5 dosyada +354/-16; bu rapor 76 yeni satırdır. Sayı yeni, henüz izlenmeyen test ve rapor dosyalarını da içerir. Kullanıcının mevcut `README.md` değişikliği ve iki inceleme raporu bu sayıya dahil edilmedi ve korundu. Commit, push, tag ve release oluşturulmadı.

Son durum: **PASS**. İstenen manifest traversal/izinsiz overwrite regresyonları, normal resume, mevcut ilgili testler ve tam CTest paketi doğrulandı. Bu sonuç, yukarıda açıklanan eşzamanlı yerel süreç yarışlarının tamamen giderildiği anlamına gelmez.

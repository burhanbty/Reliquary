# RELIQUARY — Development Task 02: Legacy Decoder Data Loss Prevention

Tarih: 28 Ekim 2026. Yalnız sentetik verilerle yerel doğrulama.

**31 Ekim 2026 kapanışı: PASS — doğrulanan yerel görev kapsamı.** Son tam
CTest: **663 PASS / 0 FAIL / 2 SKIPPED**; bu iki symlink senaryosu değişmeyen
kontrol koduyla ayrı UAC çalışmasında **2/2 PASS** verdi. Önceki PARTIAL
kararının kalan maddeleri için 30–31 Ekim'de ek çalışma yapıldı.
En güncel bulgular ve kapanış sonucu Bölüm 8'dedir;
Bölüm 1–7, 28 Ekim'de elde edilen sonuçları ve sınırlamaları korur.

## 1. Başlangıç durumu ve kapsam

İlk `git status --short` çıktısında yalnız kullanıcının iki izlenmeyen raporu vardı:
`docs/ASTRA_YENIDEN_INCELEME_RAPORU_2026-10-27.md` ve
`docs/PROJE_INCELEME_RAPORU_2026-10-27.md`. Bu dosyalara dokunulmadı.

Başlangıç HEAD'i `67f24c1b769c0951210cf027b3500ab04de1b5a4`, Task 02'nin legacy
düzeltmesini, 66 regresyonunu ve önceki tarihli raporunu zaten içeriyordu. Task 01
`a0bd774` ile bunun öncesindeydi. Bu oturumda bu commit'ler oluşturulmadı veya
geri alınmadı; mevcut düzeltme denetlendi, iki eski hata yeniden üretildi,
Fast Local yazma yolu tamamlandı ve testler genişletildi. Commit, push, tag veya
release yapılmadı.

Format, manifest şeması, şifreleme algoritması, C API imzaları ve yapı alanları
değiştirilmedi. Eksik terminal işareti ve farklı dosya kimliklerinin karışması
bu görevin dışında tutuldu.

## 2. Kök nedenler ve düzeltme öncesi yeniden üretim

**Sorun A:** Eski `Decoder::write_assembled_file()` nihai hedefi, şifreli
chunk doğrulaması tamamlanmadan truncation ile açıyordu. Sonraki parola hatası
mevcut içeriği geri getirmiyordu. Yazma/flush/close işlemleri bir yayınlama
işleminin ön koşulları olarak ele alınmıyordu.

**Sorun B:** Eski `ms_decode()` kaynak ve hedef dosya kimliğini karşılaştırmıyordu.
Video okunduktan sonra aynı dosya kurtarılmış içerikle değiştirilebiliyordu.

Bu oturumda, Task 02 öncesinden kalan `build-resume-security/compiled` CLI/DLL
çifti, yeni ve yalnız bu deney için oluşturulan sentetik dosyalarla çalıştırıldı.
Güncel kaynaklar geri alınmadı; eski uygulama yeniden derlenmiş gibi sunulmuyor.
İkili SHA-256 değerleri ve deney çıktıları
`build-recovery-safety/revalidation-20261028/original-bugs-results.json` içinde.

| Deney | Eski uygulamanın gerçek sonucu |
|---|---|
| Yanlış parola + mevcut hedef | CLI çıkışı 1; 18 baytlık sentinel 0 bayta düştü. |
| Yanlış parola + yeni hedef | CLI çıkışı 1; istenmeyen boş nihai dosya oluştu. |
| Aynı kaynak ve hedef yolu | CLI çıkışı 0; 84.926 baytlık video 43 baytlık kurtarılmış içerikle değişti. |

Sentinel SHA-256:
`BEFE935F0E46C69AFAF159E85034748E6D12696FD337612952DF6AE47F5FBED8`
→ `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`.

Aynı-yol video SHA-256:
`B95C39C3859C208EB064831FFD274430D97F0750848E2C11240F9E7E639B7A91`
→ `479FDCFB7EF45944A9036ACCFFD9522059816326F6F4943775B027BB13A4A620`.

Tekrarlama betiği: `build-recovery-safety/revalidation-20261028/reproduce-original.ps1`.
Bu betik yalnız kendisinin oluşturduğu yeni sentetik dosyalar üzerinde çalışır.

## 3. Güvenli yazma ve kaynak koruması

Mevcut legacy düzeltmesinde `SafeOutputFile::Mode::ExclusiveWrite` hedefin kendi
dizininde kısa ve rastgele isimli bir geçici dosyayı `std::ios::noreplace` ile
exclusive olarak oluşturur. Aynı stream oluşturma, yazma, flush ve close boyunca
tutulur. Ad çakışması başka dosyayı truncate etmez. Şifreli chunk'ların mevcut
AEAD doğrulaması başarısız olursa nihai hedefe dokunulmaz.

Her write, açık flush ve açık close denetlenir. Çıktı boyutu ve son iptal kontrolü
yayınlamadan önce yapılır. Windows yayınlaması aynı dizinde
`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` kullanır; hedef önceden silinmez
ve cross-volume kopyala/sil fallback'i etkinleştirilmez. POSIX dalı `rename`
kullanır; bu oturumda POSIX çalıştırılmadı. Normal hata ve iptal yollarında RAII
geçici dosyayı kapatıp kaldırır. Başarılı mevcut-hedef değiştirme davranışı korunur.

`output_conflicts_with_input()` mevcut hedef için `std::filesystem::equivalent`
ile fiziksel dosya kimliğini denetler. Bu kontrol string karşılaştırması değildir.
`ms_decode()` başlangıçta, decoder/progress açılmadan aynı kaynak için
`MS_ERR_INVALID_ARGS` döndürür. Yayınlamadan hemen önce, son progress callback'inden
sonra tekrar kontrol eder. Hardlink'in işlem sırasında hedefe eklenmesi de
reddedilir. Başlangıçta bulunmayan hedef kontrol hatası sayılmaz; mevcut dosyaların
metadata hataları C API dışına istisna taşırmaz.

Legacy parola/son yazma hatalarında mevcut `MS_ERR_DECODE_FAILED`, eksik chunk
için `MS_ERR_INCOMPLETE`, parola yoksa `MS_ERR_CRYPTO` davranışı korunur.
Fast Local crypto/corrupt/incomplete hataları kendi türleriyle korunur;
yazma ve yayınlama altyapısı hataları `MS_ERR_IO` olarak döner.

### Bu oturumda tamamlanan Fast Local yolu

`src/fast_local_codec.cpp` daha önce `PathOnly` ile seçilen geçici adı ayrı bir
`ofstream` ile yeniden açıyordu. Bu aralık exclusive değildi; ortak writer'ın
create/write/flush/close/publish hata enjeksiyonu da bu yolda çalışmıyordu.
Şimdi decode, legacy ile aynı sahip olunan exclusive stream'i kullanır ve
`finish_writing()` başarılı olmadan commit yapamaz. Encode yolu değiştirilmedi.

Ayrıca Fast Local sonuç istatistikleri için `file_size(video_path)` yayınlamadan
sonra çağrılıyordu. Bu sorgu hata verseydi hedef değiştirilmişken API başarısızlık
bildirebilirdi. Kaynak boyutu artık kurtarmanın başında alınır; commit sonrasında
yalnız önceden bilinen değerlerden istatistik üretilir. Bu sıralama kod
incelemesiyle doğrulandı; yayınlama sonrası gerçek metadata/donanım arızası deneyi
yapıldığı iddia edilmiyor.

## 4. Kaynak dosyalar ve kalıcı regresyonlar

| Dosyalar | Durum ve işlev |
|---|---|
| `src/decoder.cpp`, `src/decoder.h` | Başlangıç HEAD'inde mevcut: güvenli legacy yazma, son yayınlama kontrolü, yayınlama öncesi çıktı boyutu. Bu oturumda değiştirilmedi. |
| `src/safe_output.cpp`, `src/safe_output.h` | Başlangıç HEAD'inde mevcut: exclusive geçici stream, I/O denetimleri, kimlik yardımcısı, test hook'ları. Bu oturumda değiştirilmedi. |
| `src/media_storage_api.cpp`, `include/media_storage.h` | Başlangıç HEAD'inde mevcut: kaynak çakışması, son iptal kontrolü ve C API hata sınırı. Bu oturumda değiştirilmedi. |
| `src/fast_local_codec.cpp` | Bu oturumda: exclusive writer, ayrı flush/close, I/O hata dönüşümü, kaynak boyutunun commit öncesinde alınması. |
| `tests/test_recovery_output_safety.cpp` | Bu oturumda: I/O matrisi 10'dan 60'a, iptal matrisi 4'ten 24'e çıkarıldı; fixture okumalarının açılışı denetleniyor. |
| `tests/test_legacy_recovery_output.cmake`, `tests/CMakeLists.txt` | Mevcut CLI/test kayıtları korundu; Task 01 testleri kaldırılmadı. |
| Bu rapor | İstenen 28 Ekim tarihli yeni Türkçe sonuç raporu. |

Güncel güvenlik grubu **128 C++ + 8 CLI = 136 test** içerir. Bu oturumdaki net
artış **70 testtir**. Create/write/flush/close/publish × mevcut/yeni hedef ×
Resilient/High Capacity/Fast Local × şifreli/şifresiz kombinasyonları 60 I/O
testiyle kontrol edilir. İptal matrisi aynı altı profilde mevcut/yeni hedefi ve
erken/geç iptali kapsar. Geç iptal için geçici dosyada gerçekten bayt bulunması
beklenir; sadece dosyanın oluşturulmuş olması yeterli sayılmaz.

Fast Local'ın yeni 20 I/O testi üretim düzeltmesinden önce çalıştırıldı:
**0 PASS / 20 FAIL**. İlgili hook'lar eski Fast Local writer'ına ulaşmadığı için
işlem başarıyla yayınlıyordu. Bu, gerçek disk arızası üretimi değildir; yeni
hata enjeksiyonu kapsamının önceki yolda bulunmadığını gösterir. Düzeltmeden sonra
aynı beklentilerle testler geçti. Kanıt: `fastlocal-red.log` ve
`fastlocal-red-results.xml`.

Yeni ve mevcut hedef için yanlış parola, AEAD/CRC bozulması, terminal chunk varken
önceki chunk'ın eksikliği, hatalı şifreli başlık, iptal ve I/O hataları kalıcı
testlerde kapsanır. API/CLI güvenlik testlerinde kaynak video ve mevcut sentinel
SHA-256 değerleri karşılaştırılır; başarısız yeni-hedef işleminde nihai dosyanın
olmadığı, normal hata temizliğinde geçici dosyanın kalmadığı denetlenir.
Doğrudan writer testlerinde kaynak video bulunmadığından yalnız hedef denetlenir.

Gerçek dosya sistemi hata testleri de vardır: yazılamayan parent yolu ve
Windows'ta delete paylaşımı olmayan açık hedef handle'ı ile yayınlama hatası.
Başarılı değiştirme testleri doğru içeriği ve kaynak hash'ini; hedefin başka
hardlink'inin eski içeriği koruduğunu ayrıca doğrular.

## 5. Bu oturumdaki derleme ve test sonuçları

Windows x64, MSVC 19.44, C++23, Release; `BUILD_GUI=ON`, `BUILD_TESTS=ON`.
CLI, DLL, Qt GUI ve test hedefleri derlendi. Kanıt:
`build-recovery-safety/revalidation-20261028/build-final.log`.

İlk sandbox çalışması encode için sistem geçici dizinine yazamadı. TEMP/TMP
proje altındaki yalnız sentetik test dizinine yönlendirildi. Sonraki sandbox
çalışmasında hardlink/junction oluşturma erişim hataları görüldü. İzin verilen
sandbox dışı çalışmada aynı beklentilerle tekrar sınandılar ve geçtiler.
Bu ara başarısızlıklar loglarda korundu; başarılı doğrulama olarak sayılmadı.

Kartlar etkinken ayrı grup çalışmaları:

| Grup | Toplam | PASS | FAIL | SKIPPED |
|---|---:|---:|---:|---:|
| Kurtarma güvenliği regresyonları | 136 | 134 | 0 | 2 |
| Decoder: Codec/DCT/Stream | 51 | 51 | 0 | 0 |
| C API | 27 | 27 | 0 | 0 |
| Roundtrip/kripto/integrity | 47 | 47 | 0 | 0 |
| Fast Local/High Capacity/Video Set/Instant Recovery ve CLI | 90 | 90 | 0 | 0 |
| Video laboratuvarı | 26 | 26 | 0 | 0 |
| Qt GUI/model/dil/görsel kimlik/sonuç kartları, ilk GPU açık grup | 83 | 82 | 1 | 0 |
| Tam paket, kısa proje TEMP yolu ile ilk GPU açık çalışma | 661 | 658 | 1 | 2 |
| Tam paket, standart kullanıcı TEMP yolu ile son çalışma | 661 | 658 | 1 | 2 |

Grup satırları ayrı çalıştırmalardır; tam paket sayısına eklenmezler. Kaynak/ikili
SHA-256 kaydı `validated-files-sha256.json` içindedir.

### Ekran kartları etkin durumdayken tekrar doğrulama

Kullanıcının ekran kartının önceki çalışmada kapalı olduğunu bildirmesi üzerine
donanım durumu yeniden okundu. 28 Ekim 2026, 12:07 Türkiye saati itibarıyla
NVIDIA GeForce RTX 4070 Laptop GPU ve Intel Iris Xe için Windows `Status=OK`,
`ConfigManagerErrorCode=0` bildirdi. Kanıt: `post-gpu-environment.json`.
Bu, iki kartın etkin olduğunu gösterir; belirli testin hangi GPU'da çizildiğini
veya donanım hızlandırmasının kullanıldığını kanıtlamaz.

Video encoder/decoder akışı ve laboratuvar simülasyonları incelendi. İlgili
varsayılan FFV1/libx264 kodlamaları yazılımsaldır; bu testlerde NVENC/CUDA/QSV
seçimi yapılmıyor. Qt görüntü/önizleme/pano testleri masaüstü ortamından
etkilenebildiği için kartlar etkinken GUI grubunun tamamı ayrıca tekrarlandı.
Sürücü, ekran kartı veya sistem politikası ayarı değiştirilmedi.

İlk tam paket uzun TEMP yoluyla **652 PASS / 7 FAIL / 2 SKIPPED** verdi;
başarısızlıklar yedi `TestLabTest` senaryosundaydı. Kaynak beklentileri
değiştirilmeden kısa `C:\Users\Burhan\VidStoreX\build-rs-tmp` yolu ile iki
örnek laboratuvar testi geçti. Ayrı sandbox çalışmasında panodan boş görüntü
okuyan sonuç kartı testi de sandbox dışında tekrarlandığında geçti; bu iki
laboratuvar testi + 18 sonuç kartı testi birlikte **20/20 PASS** verdi.
Kanıt: `environment-retry-tests.log`, `environment-retry-results.xml`.
Bu karşılaştırma ortam etkisini gösterir; ilk hataların ekran kartı arızasından
kaynaklandığı iddia edilmiyor. Ara başarısızlıklar ve kesintiye uğrayan çalışmalar
son tam paket sonucuna başarılı test olarak eklenmedi.

Kartlar etkin durumdayken son çalışmanın ayrı dosyaları `*-post-gpu-tests.log`,
`*-post-gpu-results.xml` ve `group-summary-post-gpu.json` adlarıyla saklanır.

İlk GPU açık GUI grubunda `ResultCardRenderer.RecoveryIsDeterministicOpaqueBrandCanvas`
testi, 500 ms eşiğinde 899 ms ölçtüğü için başarısız oldu. Görüntü boyutu,
pixel/opacity ve deterministik içerik beklentileri hata vermedi. Eşik
değiştirilmeden, diğer GUI testleri bittikten sonraki **beş tekrarın tamamı geçti**
(test süresi 0,21–0,37 saniye). Kanıt: `renderer-repeat-tests.log`.
Bu geçici performans sapması yok sayılmıyor; kararlı GPU performansı garantisi
verilmiyor. İlk GPU açık tam pakette aynı çizim testi geçti.

İlk GPU açık tam pakette tek hata
`TestLabTest.Quick1080pRepairProfilesProduceReadyCandidates` oldu: mevcut
`manifest.json` yayınlanırken Windows `Access is denied` döndürdü. Libx264 logu
CPU kodlamasını gösteriyordu. Aynı laboratuvar testi beklentiler değiştirilmeden
**beş tekrarın tamamında geçti**; kanıt `lab-repeat-tests.log`. Kilitleyen süreç
veya geçici erişim reddinin kesin nedeni belirlenmedi; GPU'ya bağlanmıyor.
Bu manifest yayınlama yolu bu oturumun kaynak diff'inde değiştirilmedi.

Son tam paket, kartlar yine `OK/0` durumundayken standart
`C:\Users\Burhan\AppData\Local\Temp` dizininde ayrıca çalıştırıldı. Ortam kaydı
`system-temp-environment.json`, sonuçlar `full-system-temp-tests.log` ve
`full-system-temp-results.xml` içindedir. **283,18 saniyede 658 PASS, 1 FAIL,
2 SKIPPED**. Önceki manifest hatası ve çizim testi bu tam çalışmada geçti.
Kalan hata `GUI.YouTubeSyncE2E`: uygulamanın 120 saniyelik dahili smoke deadline'ı
doldu ve çıkış kodu 96 döndü (CTest süresi 122,27 saniye). Bu test daha önceki
GPU açık GUI grubunda geçmişti. Test yerel fake API/sentetik kimlik bilgileri
kullanır; gerçek hesaba upload yapılmadı. Hatanın GPU kaynaklı olduğu
kanıtlanmadı; bu oturumda YouTube Sync üretim kodu veya timeout'u değiştirilmedi.
Sonuç **FAIL** olarak korunur; önceki başarılı grup sonucu bu hatanın yerine
sayılmadı. Makinece okunabilir son özet `final-summary.json` içindedir.
Sentetik timeout sidecar'ı `youtube-sync-timeout-state.json` olarak korundu:
fake playlist oluşturulmuş, ilk parça `session_created` durumunda ve
`uploaded_bytes=0`; diğer üç parça beklemede. Bu kayıt timeout'un görüntü
doğrulamasından önce sync akışında kaldığını gösterir; kesin kök nedenini kanıtlamaz.

Tekrar çalıştırma (hardlink/junction ve pano erişimini destekleyen ortamda):

```powershell
cmd /c build-recovery-safety\build.cmd
New-Item -ItemType Directory -Path build-rs-tmp -Force | Out-Null
& build-recovery-safety/revalidation-20261028/verify.ps1 -Final -RunTag rerun
```

Standart kullanıcı TEMP yolundaki son tam paket ayrıca `ctest --test-dir
build-recovery-safety/compiled --output-on-failure` ile, TEMP/TMP bu kullanıcı
Temp dizinine ayarlanarak çalıştırıldı. Tekil tekrarlar `--repeat until-fail:5`
kullanır; bunlar beş ayrı yeni CTest kaydı gibi toplam test sayısına eklenmedi.

Hata enjeksiyonu yalnız test makrosuyla etkinleşir. `safe_output.cpp`, test
makrosu olmadan ayrı mini programda tekrar derlenip beş hata ortam değişkeniyle
çalıştırıldı: **PASS**. Bu smoke ayrı bir tam `BUILD_TESTS=OFF` uygulama derlemesi
olarak sunulmuyor. Kanıt: `production-smoke.log`.

## 6. Uyumluluk ve kalan riskler

Legacy single-video, High Capacity, Resilient, Fast Local, Video Set, Instant
Recovery, CLI/C API ve Qt GUI testleri bu oturumda yeniden çalıştırıldı. Mevcut
format/şema/şifreleme korunmuştur. Task 01 üretim dosyaları ve testlerine bu
oturumda diff yoktur. Başarı formatın mevcut kontrollerinden geçme anlamındadır;
legacy şifresiz veriye yeni bir uçtan uca kaynak hash alanı eklenmedi.

- **Dosya symlink'i: SKIPPED.** Giriş ve hedef için iki dosya symlink testi,
  Windows `ERROR_PRIVILEGE_NOT_HELD` (1314) nedeniyle çalışmadı. Unprivileged
  symlink bayrağıyla ayrı oluşturma denemesi de 1314 döndürdü. Geçen junction
  testleri bu iki testin yerine sayılmadı. Sistem politikası değiştirilmedi.
- **TOCTOU:** Başlangıç/son kimlik kontrolleri ve exclusive geçici dosya yarışı
  daraltır. Codec ve filesystem yolları bütünüyle açık dizin/dosya handle'larına
  bağlanmadığından eşzamanlı parent/link değişimi veya son kontrol ile rename
  arasındaki yarış bütünüyle giderilmedi.
- **Kalıcılık:** Flush/close kontrolleri güç kesintisi için tam fsync/
  FlushFileBuffers ve dizin kalıcılığı protokolü değildir. Güç kesintisi,
  dosya sistemi sağlayıcı arızası, ağ diskleri ve süreç çökmesi test edilmedi.
- **Temizlik:** Normal hata/iptalde RAII temizliği doğrulandı. Harici kilit,
  izin değişimi veya süreç çökmesinde best-effort kaldırma başarısız olabilir;
  geçici plaintext dosyası kalabilir.
- **Metadata:** Başarılı rename eski hedefin ACL/ADS ve hardlink ilişkilerini
  koruma taahhüdü vermez. Diğer hardlink'in içeriğinin korunması sınandı.
- **Platform:** Linux/macOS çalıştırılmadı. Donanım disk hataları enjekte edilmiş
  stream hata durumlarından ayrı tutulur; gerçek disk doldurulmadı.

## 7. Git diff ve karar

Bu oturumun çalışma ağacı değişiklikleri yalnız `src/fast_local_codec.cpp`,
`tests/test_recovery_output_safety.cpp` ve bu yeni rapordur. Önceden mevcut iki
kullanıcı raporu izlenmeyen olarak korunmuştur. Önceki tarihli Task 02 raporu
silinmedi/değiştirilmedi. Görev dışı refactoring yapılmadı.

İki kaynak/test dosyasının HEAD'e göre diff'i **+35/-37** satırdır; bu yeni rapor
ayrıca eklenmiştir. `git diff --check` geçti. Task 01 üretim/test dosyaları ve
README için çalışma ağacı diff'i boş. Test beklentileri, süre eşikleri ve
test kayıtları gevşetilmedi veya kaldırılmadı.

**Genel karar: PARTIAL.** İki eski veri kaybı sentetik dosyalarla yeniden
üretildi. Mevcut legacy düzeltmesi korundu, Fast Local güvenli writer'a geçirildi
ve 70 ilave regresyon eklendi. Çalıştırılabilen **134/134 kurtarma güvenliği
testi geçti**: yanlış parola, algılanabilir bozulma, eksik veri, iptal ve
enjekte edilen I/O hatalarında mevcut hedef/kaynak SHA-256 değerleri korundu;
başarısız yeni-hedef işlemleri nihai dosya bırakmadı.

İki zorunlu dosya symlink senaryosu ayrıcalık eksikliğinden doğrulanamadı.
Son tam CTest'teki **1 GUI YouTube Sync timeout hatası** da çözülmüş veya PASS
sayılmadı. Bu nedenle tüm uyumluluk kapılarının temiz geçtiği ya da koşulsuz
veri koruma garantisi verildiği iddia edilmiyor. TOCTOU, güç kesintisi ve
best-effort temizlik sınırları yukarıda belgelendi. Ekran kartları etkin durumdaki
yeniden çalıştırmalar önceki sorunların GPU kaynaklı olduğunu kanıtlamadı.

## 8. 30–31 Ekim 2026 — Kalan symlink ve GUI doğrulaması

### 8.1. Çalışma ağacı ve izin sınırı

Başlangıç HEAD'i yine `67f24c1b769c0951210cf027b3500ab04de1b5a4` idi.
28 Ekim'de bu chat içinde yapılan Fast Local ve güvenlik testi değişiklikleri,
bu rapor ve kullanıcının iki izlenmeyen raporu korunarak devam edildi.
Task 01 güvenlik kuralları, formatlar, C API/ABI ve kullanıcı dosyaları korundu.
Ek doğrulamada ortaya çıkan geçici klasör-yayınlama erişim reddi için, yalnız
aynı rename işlemini güvenlik kontrolleriyle tekrar eden dar bir değişiklik
yapıldı (Bölüm 8.6). Commit, push ve release yapılmadı.

Symlink oluşturma mevcut yönetici olmayan token ile tekrar denendi ve yine
ayrıcalık hatası verdi. Kullanıcı, **yalnız iki sentetik symlink testinin UAC
ile yönetici olarak çalıştırılmasını** açıkça onayladı. Yönetici runner'ı
yalnız `Paths/RecoveryAlias.*symlink` filtresini çalıştırdı; sistem politikası,
Developer Mode, antivirüs veya kullanıcı hakları değiştirilmedi. Tam uygulama
ve tam CTest paketi yönetici olarak çalıştırılmadı.

### 8.2. Symlink doğrulaması: iki eksik senaryo kapandı

`symlink` ve `input_symlink` senaryoları yönetici test ortamında **2 PASS,
0 FAIL, 0 SKIPPED** verdi. Gerçek dosya symlink'i oluşturuldu ve
`filesystem::equivalent` ile kaynak/hedef fiziksel kimliği doğrulandı.
Her iki API çağrısı progress başlamadan `MS_ERR_INVALID_ARGS` ile reddedildi;
kaynak ve linkten okunan SHA-256 değişmedi; normal fixture temizliği tamamlandı.
Hardlink veya junction sonucu bu testlerin yerine sayılmadı.

Kanıt klasörü: `build-recovery-safety/completion-20261030/`.
Dosyalar: `run-symlinks-elevated.ps1`, `symlink-elevated-tests.log`,
`symlink-elevated-results.xml`, `symlink-elevated-status.json`.

Yönetici olmayan tam pakette aynı iki testin SKIPPED olması bir yetki
sınırlamasıdır. Bu kayıtlar PASS'e dönüştürülmedi; ayrı yönetici çalışmasındaki
iki gerçek PASS ile gereksinim kapsamı tamamlandı.

### 8.3. GUI fake API'de doğrulanan veri-hazır sinyali yarışı

`FakeYouTubeApi`, kabul edilen sokette yalnız gelecekteki `readyRead`
sinyaline bağlanıyordu. İstek baytları handler bağlanmadan önce Qt tamponuna
gelmişse bu sinyal tekrar verilmez; ek bayt gelmediğinde istek yanıtsız kalır.
Upload-session POST yanıtı da gelmeyince GUI beklemeye devam eder.

Bu davranışı aynı fixture ile kalıcı olarak sınamak için sınıf
`src/main_gui.cpp` içinden `src/test_fake_youtube_api.h` dosyasına çıkarıldı.
Header GUI'de yalnız `VIDSTOREX_ENABLE_TEST_HOOKS` altında, ayrıca ağ test
hedefinde kullanılır. Bu taşıma, testin farklı bir sahte sunucu kopyasını
sınamasını önler; üretim OAuth/uploader kodu değiştirilmedi.

Yeni `YouTubeNetworkFakeHttp.GuiUploadSessionConsumesAlreadyBufferedRequest`
regresyonu, normal istek handler'ını yalnız baytlar zaten tampondayken bağlar.
Önce handler bağlanmadan veri gelme durumu 50 ms geciktirme ile yeniden
üretildi: **1 FAIL**, HTTP status 0 ve eksik Location; tamponda bayt bulunduğu
assertion'ı geçti. Yerel ağın engellendiği sandbox denemesi ayrıca loglandı ve
bu yeniden üretimin kanıtı olarak sayılmadı. Son test sürümünde sabit süreye
bağımlılık kaldırılarak sıralama `readyRead` üzerinden zorlanır.

Düzeltme handler bağlandıktan hemen sonra `consume(socket)` çağırır; böylece
hem mevcut tampon hem gelecekteki sinyaller işlenir. Bağlantı kapandığında
fixture tamponu temizlenip soket `deleteLater` ile bırakılır; geciktirilmiş
test bağlantısı `QPointer` ile soket ömrünü denetler. Regresyon aynı HTTP 200
ve `/session/1` Location beklentileriyle **20/20 tekrarda PASS** verdi.
Kanıt: `buffered-red-permitted-tests.log`, `buffered-green-tests.log` ve XML'ler.

Ara build hataları da saklandı: ilk header taşımasında Qt include'u anonim
namespace içindeydi; include üst seviyeye alındı. Sonraki teşhis build'i hâlâ
çalışan test GUI'sine link edemedi; test çıkınca yeniden link edildi. Bunlar
başarılı build veya üretim regresyonu olarak sayılmadı.

### 8.4. GUI hata teşhisi ve uçtan uca tekrarlar

İlk HTTP düzeltmesi sonrasındaki bir GUI denemesi iki parça ilerledikten sonra
yine 120 saniyelik deadline'a ulaştı. O denemenin son UI hata metni
kaydedilmediğinden kesin nedeni belirlenemedi; HTTP yarışı bütün geçmiş
timeout'ların tek açıklaması olarak sunulmuyor. Yanlış biçimde başarıya
çevrilmedi: `gui-sync-repeat-tests.log` ve `gui-first-repeat-state.json` korundu.

Bunun üzerine test hook'una hata durumunu `sync-timeout.txt` içine kaydetme
eklendi. GUI açıkça `needs attention` bildirirse smoke artık deadline'ı
beklemeden başarısız olur. CMake fixture'ı bu metni hata çıktısına ekler.
Başarı için playlist, tüm parçaların upload/processing durumu, auth/gizlilik
invariant'ları, progress 100 ve sonuç ekranı şartları aynen korunur.
120/150/180 saniyelik mevcut timeout'lar artırılmadı.

Son teşhis koduyla tek GUI denemesi geçti, ardından
`GUI.YouTubeSyncE2E --repeat until-fail:10` **10/10 PASS** verdi
(yaklaşık 2,39–2,75 saniye/çalışma; toplam 25,46 saniye).
Her çalışma CLI ile dört yeni sentetik Video Set parçası oluşturup yalnız
127.0.0.1 üzerindeki fake API'ye gönderdi. Gerçek YouTube hesabı veya bulut
isteği kullanılmadı. Kanıt: `gui-diagnostic-tests.log`,
`gui-diagnostic-repeat-tests.log` ve karşılık gelen XML'ler.

### 8.5. Instant Recovery fixture izolasyonu

İlk 30 Ekim grup turunda GUI **82 PASS / 1 FAIL** verdi; YouTube Sync bu grupta
geçti. Kalan hata `GUI.InstantPlaylistRecoveryE2E` hazırlığındaki CLI encode
adımıydı: dört parça yerel olarak doğrulandıktan sonra staging dizininin nihai
test dizinine rename işlemi Windows `Access is denied` ile reddedildi. İlk tam
paket aynı kaynaklarla **660 PASS / 0 FAIL / 2 SKIPPED**, 173,22 saniye verdi;
ancak ek Instant tekrarında aynı staging-yayımlama hatası yeniden görüldü.
Başarısız test başarı olarak sayılmadı (`gui-tests.log`, `instant-retry-tests.log`).

Yalnız testin ürettiği staging dizini containment kontrolüyle tekrar aynı
test parent'ı içinde rename edildi; 7 ms'de başarılı oldu. Bu, erişim reddinin
kalıcı olmadığını gösterir; kilitleyen süreç tespit edilmiş gibi sunulmuyor.
Qt ortamı taşınmadan yapılan bağımsız bir probe GUI başlangıcında beklediği
için sonlandırıldı; PASS sayılmadı. Ardından CTest'in ENVIRONMENT ve
WORKING_DIRECTORY özellikleri aynen kullanılarak üç taze/kısa sentetik root'ta
aynı senaryo **3/3 PASS** verdi (`instant-canonical-isolated-*.log`).

`tests/test_gui_instant_recovery.cmake` artık sabit fixture dizinini recursive
silip yeniden kullanmak yerine aynı test parent'ında kısa ve rastgele isimli
yeni bir dizin oluşturur; ad çakışırsa hata verir. Önceki başarısız fixture'lar
kanıt olarak kalır. İçerik boyutu, dört parça, profil, hash karşılaştırması,
görsel/pano kontrolleri ve timeout'lar değiştirilmedi. Bu değişiklik üretim
rename politikasını veya Task 01 kontrollerini gevşetmez; testler arası dizin
yeniden kullanımını kaldırır. Yeni fixture ile CTest tekrarı **3/3 PASS** verdi
(9,19–9,54 saniye; `instant-isolated-green-tests.log`).

### 8.6. Kalıcı ve geçici Windows dizin kilitlerinin ayrılması

Fixture izolasyonu sonrasında Instant Recovery'nin üç tekrarı geçse de takip
eden tam pakette yeni dizinde aynı rename erişim reddi yeniden oluştu. Bu
nedenle izolasyon tek başına kök neden düzeltmesi olarak sunulmuyor.
O tur **659 PASS / 1 FAIL / 2 SKIPPED** verdi; hata Instant fixture encode
adımıydı. Aynı tura başlayan eski Assistant fixture'ı da oluşturma akışında
kod 32 ile başarısız oldu. Bu ara kayıtlar
`*-before-directory-retry-tests.log` ve XML'lerde korunur.

`src/directory_publish.h`, yeni hedefe klasör yayınlarken kullanılan mevcut
`filesystem::rename` işlemini Windows'un `ERROR_ACCESS_DENIED` veya
`ERROR_SHARING_VIOLATION` hatalarında en fazla 10 kez, 20 ms arayla tekrar eder.
Bu, bekleme kısmı için en fazla 200 ms'dir; güvenlik ağacı tarama süresi ayrıca
vardır. Diğer hatalar hemen, kalıcı kilit ise tekrar sınırında hata döner.
Hedefi silme, kopyalama fallback'i, ACL/antivirüs değişikliği yoktur.

`src/video_set_cli.cpp` bu yardımcıyı yalnız henüz bulunmayan hedefe bir
**dizin** yayınlama dalında kullanır. Her deneme öncesinde staged ve hedef
ağaçları Task 01 kontrollerinden tekrar geçer; hedef arada ortaya çıkarsa
yayınlama reddedilir. Mevcut hedefi backup ile değiştiren dal ve legacy dosya
writer'ının yayınlama davranışı değiştirilmedi. Böylece bekleme, güvenlik veya
overwrite kontrolünü atlayan yeni bir yol oluşturmaz. Tam TOCTOU giderimi
sağlandığı iddia edilmiyor.

Üç kalıcı Windows regresyonu eklendi: delete paylaşımı olmayan gerçek dizin
handle'ı ilk rename'i engeller; ikinci doğrulamada handle kapatılınca aynı
atomic rename başarıyla tamamlanmalıdır. Handle sürekli açık kaldığında işlem
başarısız olmalı, staging SHA-256 korunmalı ve hedef oluşmamalıdır. Yeniden
doğrulamada hedefin oluştuğu senaryoda ise yayınlama reddedilmeli; hem staging
hem yeni hedef korunmalıdır. Kilidin bırakılması deneme sayısına bağlıdır;
rastgele zamanlanmış bir sleep başarı varsayımı olarak kullanılmaz.

### 8.7. Son doğrulama ve karar

Son Release build'i CLI, DLL, Qt GUI ve test hedefleriyle başarılı:
`build-directory-retry.log`. Ağ regresyonu ve üç dizin-yayınlama testiyle
paket **665 benzersiz testtir**. Tekrar sayıları bu sayıya eklenmez.

| Son tam paketten test görünümü | Toplam | PASS | FAIL | SKIPPED |
|---|---:|---:|---:|---:|
| Kurtarma güvenliği, CLI ve yeni dizin yayınlama regresyonları | 139 | 137 | 0 | 2 |
| Sahte HTTP ağı | 3 | 3 | 0 | 0 |
| Decoder: Codec/DCT/Stream | 51 | 51 | 0 | 0 |
| C API | 27 | 27 | 0 | 0 |
| Roundtrip/kriptografi/integrity | 47 | 47 | 0 | 0 |
| Fast Local/High Capacity/Video Set/Instant Recovery ve CLI | 90 | 90 | 0 | 0 |
| Video laboratuvarı | 26 | 26 | 0 | 0 |
| Qt GUI/model/dil/görsel kimlik/sonuç kartı | 83 | 83 | 0 | 0 |
| **Tam CTest** | **665** | **663** | **0** | **2** |
| **Ayrı UAC symlink çalışması — yukarıdaki iki senaryo** | **2** | **2** | **0** | **0** |

Son GUI grubu ayrıca **83/83 PASS** verdi. Son tam paket **162,65 saniyede**
tamamlandı. `GUI.YouTubeSyncE2E`, `GUI.InstantPlaylistRecoveryE2E` ve
`GUI.VideoSetAssistantE2E` bu son tam pakette de geçti. Önceki bir başarısız
grup sonucu sonucun içinden silinmedi; son kaynaklarla tekrar doğrulandı.

Yeni üç Windows dizin-kilidi testi beşer tekrarda **15/15 başarılı çalıştırma**
verdi (`directory-retry-tests.log`); bunlar tabloda üç benzersiz test olarak
yer alır. Ağ regresyonunun 20 ve YouTube Sync'in 10 başarılı tekrarı da ayrı
kanıtlardır; paket sayısını artırmaz.

**UAC kaydının sınırı:** İki symlink testi 30 Ekim 23:29 Türkiye saatindeki
ayrı yönetici çalışmasında gerçek PASS verdi. Son ikili için ek UAC açılışı
Windows tarafından “işlem kullanıcı tarafından iptal edildi” ile döndü;
bu, çalışmış bir test veya yeni PASS olarak sayılmadı. Yönetici olmayan son
paketin iki SKIPPED kaydı aynen korunur. Symlink regression gövdesinin HEAD ile
aynı olduğu ve `media_storage_api.cpp`, `decoder.cpp`, `safe_output.cpp/.h`
kimlik-kontrol kaynaklarının o çalışmadan beri değişmediği ayrıca doğrulandı:
`symlink-source-continuity.json`. Ayrı PASS'ler son derlemede tekrar yönetici
çalıştırılmış gibi sunulmuyor.

Her benzersiz senaryonun gerçek PASS kaydıyla kapsam eşleştirmesi
`coverage-summary-final.json` içindedir: normal tam pakette 663, ayrı yönetici
çalışmasında değişmeyen iki alias senaryosu; **kapsanmayan senaryo 0**.
Bu kayıt JUnit dosyalarını değiştirmez veya SKIPPED durumunu PASS'e çevirmez.

Son kanıtların tamamı `build-recovery-safety/completion-20261030/` altındadır:

- `gui-final-tests.log`, `gui-final-results.xml`: son ayrı GUI grubu.
- `full-final-tests.log`, `full-final-results.xml`: son 665 testlik tam paket.
- `final-groups-from-full.json`: tabloda verilen grupların son XML'den sayımı.
- `group-summary-final.json`: son iki çalışma için exit/status özeti.
- `symlink-elevated-*.xml/.log`, `symlink-source-continuity.json`: ayrı symlink kanıtı.
- `validated-final-files-sha256.json`: doğrulanmış kaynak ve ikili hash'leri;
  testlerden sonra hâlâ eşleştikleri kontrol edildi.
- `verify.ps1`, `audit-coverage.ps1`: tekrar çalıştırma ve kapsam denetimi.

**Diff:** Başlangıç HEAD'ine göre kod/testlerde **10 dosya, +383/-185 satır**
(iki yeni header dahil); bu rapor ayrıca yeni dosyadır. 28 Ekim'den kalan
Fast Local/test değişiklikleri bu sayıya dahildir. Yeni header ve rapor dahil
whitespace kontrolü geçti. Kullanıcının iki raporu korunmuştur.

| Kapanışta değişen dosyalar | İşlev |
|---|---|
| `src/fast_local_codec.cpp` | 28 Ekim'den korunmuş exclusive çıktı ve commit öncesi metadata düzeltmesi. |
| `src/main_gui.cpp`, `src/test_fake_youtube_api.h` | Paylaşılan test fixture'ı, mevcut soket baytlarını tüketme ve smoke hata teşhisi. |
| `src/directory_publish.h`, `src/video_set_cli.cpp` | Windows yeni-dizin yayınlamasında sınırlı tekrar; her denemede Task 01 kontrolü ve yeniden overwrite kontrolü. |
| `tests/test_recovery_output_safety.cpp` | Önceki 70 ek regresyon ve üç yeni gerçek-handle dizin yayınlama testi. |
| `tests/test_youtube_network.cpp` | Aynı GUI fixture'ında deterministik önceden tamponlanmış POST regresyonu. |
| `tests/test_gui_instant_recovery.cmake`, `tests/test_gui_video_set_assistant.cmake` | Her çalışma için yeni, kısa sentetik fixture dizini. |
| `tests/test_gui_youtube_sync.cmake` | Smoke hata metninin başarısız test çıktısında korunması. |

**Karar: PASS — bu görevin doğrulanan yerel Windows kapsamı.** İki eski veri
kaybı yeniden üretildi, güvenli çıktı mekanizması ve kaynak koruması korundu,
iki symlink senaryosu gerçekten çalıştırıldı, fake-HTTP yarışı kalıcı
regresyonla sınandı, geçici/kalıcı Windows dizin kilitleri ayrıldı ve son GUI
ile tam paket hata vermedi. Eski PARTIAL kararının iki eksik senaryosu kapandı.

Bu karar Bölüm 6'daki TOCTOU, güç kesintisi, harici kilit, best-effort temizlik
ve platform sınırlarını kaldırmaz. Bütün tarihsel GUI timeout'larının aynı
nedenle oluştuğu veya dışarıdan tutulan bir kilitte yayınlamanın her zaman
başaracağı iddia edilmiyor. Kalıcı kilitte beklenen davranış güvenli hata ve
verinin korunmasıdır. Format/manifest/şifreleme/C API ABI değişmedi; eksik
terminal ve farklı dosya kimliklerinin karışması sonraki görevlere bırakıldı.
Commit, push ve release yapılmadı.

## 9. Git teslim kapsamı — 31 Ekim 2026

Kapanıştan sonra verilen push yetkisiyle teslim hedefi mevcut
`origin/gelistirme` branch'i olarak belirlendi. Bölüm 1–8'deki commit/push
yapılmadığı ifadeleri, bu teslim yetkisinden önceki geliştirme durumunu anlatır.
Teslim allowlist'i, Bölüm 8.7'deki 10 kod/test dosyası ve bu rapordan oluşur.
Kullanıcının diğer iki inceleme raporu, build ürünleri ve sentetik test
çıktıları kapsam dışındadır. Kaynak ve ikili hash'leri son test kanıtlarıyla
yeniden eşleştirildi; bu teslim adımında uygulama kodu değiştirilmedi.
Uzak SHA doğrulaması push tamamlandıktan sonra ayrıca bildirilir.

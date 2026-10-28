# Legacy kurtarmada veri kaybını önleme ve güvenli çıktı yayımlama

Tarih: 27 Ekim 2026. Kapsam: RELIQUARY Development Task 02.

## 1. Kök neden

**B1:** `Decoder::write_assembled_file()`, şifreli chunk başlıklarını ve şifre çözme kimlik doğrulamasını tamamlamadan nihai hedefi `std::ofstream` ile açıyordu. Bu açılış mevcut dosyayı truncate ediyordu. Yanlış parola daha sonra hata üretse bile önceki içerik kayboluyordu. Yazma sonunda açık bir flush/close kontrolü de yoktu.

**A3:** `ms_decode()`, kaynak video ile hedefin aynı fiziksel dosya olup olmadığını denetlemiyordu. Video okuma tamamlandıktan sonra kaynak, kurtarılan veriyle değiştirilip başarı döndürülebiliyordu.

Düzeltmeden önce yeni kalıcı testlerin üçü de beklenen şekilde başarısız oldu:

| Sentetik deney | Düzeltme öncesi sonuç |
|---|---|
| Yanlış parola, mevcut 18 baytlık sentinel | Hata döndü; hedef boş dosyaya dönüştü. |
| Aynı giriş/çıkış yolu | `MS_OK` döndü; kaynak videonun SHA-256 değeri değişti. |
| Yanlış parola, yeni hedef | Hata döndü; istenmeyen boş nihai dosya oluştu. |

Sentinel SHA-256: `BEFE935F0E46C69AFAF159E85034748E6D12696FD337612952DF6AE47F5FBED8` → boş dosya SHA-256 değeri `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`.

Aynı-yol deneyindeki video SHA-256: `49E6F3487378D165988F6FC739779B2C007B6CA5FD7F1A20AB9F4E44ACD7471F` → `7ABF19E61A1072E005817F74D9288E6893AFB0EC18B19F263977BBCF8F30AC3A`.

Kanıt: `build-recovery-safety/red-regression.log` ve `red-results.xml`. Yalnız testin oluşturduğu sentetik dosyalar kullanıldı.

## 2. Uygulanan düzeltmeler

| Dosya | Değişiklik |
|---|---|
| `src/decoder.cpp` | Nihai dosyaya doğrudan yazma kaldırıldı. Exclusive geçici dosya, kontrollü flush/close, yayınlama öncesi callback ve hata temizliği eklendi. Paralel şifre çözmedeki paylaşılan hata bayrağı atomik yapıldı. |
| `src/decoder.h` | Dahili yazma metoduna varsayılanlı yayınlama kontrolü ve çıktı boyutu parametreleri eklendi. |
| `src/safe_output.h`, `src/safe_output.cpp` | Mevcut altyapıya `ExclusiveWrite` modu, sahip olunan stream, `write()` ve `finish_writing()` eklendi. Dosya sistemi eşdeğerliği yardımcısı eklendi. |
| `src/media_storage_api.cpp` | Kurtarma başlamadan kaynak/hedef kontrolü, yayınlama öncesi tekrar kontrol ve son iptal kontrolü eklendi. Dosya sistemi istisnalarının C API dışına çıkması engellendi. Legacy ve stream kurtarmada boyut, yayınlama öncesinden alınır. |
| `src/fast_local_codec.cpp` | Doğrudan Fast Local çağrılarında da kaynak eşdeğerliği kontrol edilir; yayınlama öncesinde kimlik ve iptal tekrar denetlenir. |
| `include/media_storage.h` | Var olan overwrite davranışı, aynı kaynak reddi ve legacy doğrulamanın sınırı belgelendi; yapı alanları ve C fonksiyon imzaları değişmedi. |
| `tests/test_recovery_output_safety.cpp` | 58 kalıcı C++ regresyonu eklendi. |
| `tests/test_legacy_recovery_output.cmake` | 8 kalıcı CLI regresyonu eklendi. |
| `tests/CMakeLists.txt` | Yeni testler kaydedildi; Task 01 kayıtları korundu. |
| Bu rapor | Kök neden, kanıtlar, sonuçlar ve kalan sınırlar kaydedildi. |

Task 01 üretim/test dosyaları ve kullanıcının `README.md` değişikliği korunmuştur. `src/video_set.cpp`, `src/video_set_cli.cpp`, `tests/test_video_set.cpp`, `tests/test_video_set_resume_security.cmake` ve `README.md` için başlangıç/son SHA-256 değerleri aynı bulundu. Kanıt: `protected-files-before.csv` ve `protected-files-after.csv`.

## 3. Güvenli yazma mekanizması

1. Decoder mevcut chunk tamlığı, dosya kimliği ve parola gerekliliği kontrollerini yapar.
2. Hedefin bulunduğu dizinde, kısa `.vidstorex-part-<random>-<sequence>` adıyla geçici dosya oluşturulur. `std::ios::noreplace`, mevcut bir adın truncate edilmesini engelleyen exclusive-create işlemini sağlar. Çakışmada farklı ad denenir; üst sınır 32 denemedir.
3. Stream aynı nesne tarafından oluşturulup tutulur; legacy yolunda geçici dosya ayrıca yeniden açılmaz. Şifreli içerikte mevcut başlık ve AEAD doğrulamaları tamamlanır; çözülmüş chunk'lar daha sonra geçici dosyaya yazılır.
4. Her yazma sonucu, açık `flush()` ve açık `close()` sonucu denetlenir. Herhangi bir hata yayınlamayı engeller.
5. Nihai sonuç için gereken dosya boyutu geçici dosyadan alınır. Son iptal kontrolü ve kaynak/hedef eşdeğerliği kontrolü yapılır.
6. Windows'ta mevcut `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` çağrısıyla aynı dizinde yayınlanır. Önce hedefi silen bir yol ve `COPY_ALLOWED` fallback'i yoktur. POSIX dalı aynı dosya sisteminde `rename` kullanır.
7. Yayınlama öncesindeki hata veya iptalde RAII, açık stream'i kapatır ve geçici dosyayı kaldırır. Başarılı yayınlamadan sonra hata üretebilecek dosya boyutu sorgusu yapılmaz.

| Mekanizma | İncelenen davranış ve bu görevdeki yaklaşım |
|---|---|
| Önceki legacy decoder | Hedefi erken truncate ediyor; kapanışı doğrulamıyordu. Yeni exclusive writer yalnız bu yazma yoluna uygulandı. |
| Fast Local | Geçici dosyada yazıp boyut, checksum, AEAD ve kaynak SHA-256 doğrulaması sonrasında kapatıp yayınlıyordu. Bu kontroller korundu; kaynak çakışması ve son iptal denetimi eklendi. |
| Video Set | Parça ve tam dosya SHA-256 kontrollerinden sonra yayınlıyor; açık overwrite için backup/geri yükleme yolu bulunuyor. Bu kaynak kod değiştirilmedi. |
| Eski `SafeOutputFile` kullanımları | Varsayılan `PathOnly` modu ve mevcut çağıranların davranışı korundu. Onların geçici dosya ayırmasının da exclusive hale getirildiği iddia edilmiyor. |

**Overwrite sözleşmesi:** Legacy C API'de overwrite alanı yoktur. CLI legacy decode ve GUI manuel legacy decode mevcut hedefi değiştirebiliyordu; GUI'nin encode onayı ayrı bir yoldadır. En küçük uyumlu çözüm olarak bu başarılı değiştirme davranışı korundu ve API başlığında belgelendi. Artık başarısız kurtarma overwrite yapamaz. Kaynak videoyu temsil eden hedef, overwrite davranışından bağımsız reddedilir. Video Set'in açık `--overwrite` politikası değiştirilmedi.

## 4. Kaynak-hedef koruması

Kontrol string karşılaştırmasına dayanmaz. Mevcut hedef için `std::filesystem::equivalent(input, output)` kullanılır; böylece gerçek dosya kimliği, hardlink ve bağlantılar üzerinden çözümleme dikkate alınır. Dosya sistemi metadata hatası farklı dosya kabul edilmez; işlem hata döndürür.

`ms_decode()` aynı fiziksel dosya için video decoder açılmadan ve progress callback çağrılmadan `MS_ERR_INVALID_ARGS` döndürür. Başarılı yazma sonrasında, yayınlamadan hemen önce tekrar kontrol edilir. Sentetik bir progress callback'inin kurtarma sırasında hedefte kaynak hardlink'i oluşturduğu test de yayınlama reddini doğruladı.

Çalıştırılıp geçen eşdeğerlik örnekleri: aynı yol, göreli/mutlak yol, `.`/`..`, hardlink, Windows harf büyüklüğü, DOS kısa yolu, sonda nokta ve CLI üzerinden junction. Başarılı kaynak reddi testlerinde video SHA-256 değeri değişmedi. Resilient, High Capacity ve Fast Local için şifreli/şifresiz aynı-yol reddi de test edildi.

Dosya symlink'i üzerinden giriş ve çıkış testleri bu Windows hesabında bağlantı oluşturma ayrıcalığı bulunmadığından **SKIPPED** oldu. `equivalent()` kullanan kod bu durumu kapsamak üzere yazıldı; burada symlink üzerinden dinamik doğrulama yapıldığı iddia edilmiyor.

## 5. Regresyon testleri

Yeni paket: 58 C++ + 8 CLI = **66 test**. Son grup çalışmasında **64 PASS, 0 FAIL, 2 SKIPPED**.

| Senaryo | Sonuç |
|---|---|
| Doğru parola, doğru içerik; yeni ve mevcut hedef | PASS; Resilient, High Capacity ve Fast Local, şifreli/şifresiz. |
| Yanlış parola, mevcut sentinel | PASS; sentinel ve video SHA-256 değişmedi. |
| Yanlış parola, yeni hedef | PASS; nihai dosya oluşmadı, video SHA-256 değişmedi. |
| Bozuk şifreli veri | PASS; bozuk paket CRC'si ve geçerli paket CRC'si içindeki bozuk AEAD tag'i ayrı denendi. |
| Bozuk şifresiz veri | PASS; mevcut paket checksum kontrolünün reddettiği veri kullanıldı. |
| Eksik chunk/paketler | PASS; terminal chunk mevcutken önceki chunk eksik bırakıldı; `MS_ERR_INCOMPLETE`. |
| Aynı yol ve mevcut ortamda oluşturulabilen eşdeğerler | PASS; progress başlamadan ret ve kaynak SHA-256 değişmezliği. |
| Symlink hedefi / symlink giriş | SKIPPED; Windows: `A required privilege is not held by the client.` |
| İptal | PASS; şifreli kurtarmada tarama sırasında ve geçici dosya oluşturulduktan sonra; mevcut/yeni hedef. |
| Create/write/flush/close/publish hataları | PASS; her aşamada mevcut/yeni hedef için deterministik hata enjeksiyonu. |
| Windows gerçek yayınlama hatası | PASS; hedefte delete paylaşımı olmayan açık handle ile `MoveFileExW` başarısız oldu; sentinel korundu. |
| Yazılamayan hedef yolu | PASS; gerçek dosya sistemi hatasında mevcut dosyalar korundu. |
| Mevcut hedefin başarılı değiştirilmesi | PASS; progress gözlemleri sırasında eski hash korundu, başarı sonrasında içerik doğru. |
| Hedefin başka hardlink'i | PASS; yayınlama eski inode/dosya nesnesini truncate etmedi; diğer hardlink sentinel olarak kaldı. |
| Geçici dosya yaşam döngüsü | PASS; iki ayrı exclusive dosya çakışmadı, close öncesi commit reddedildi ve normal hata yollarında artık geçici dosya kalmadı. |
| Geçersiz şifreli chunk başlığı | PASS; doğrudan decoder çağrısı mevcut hedefi korudu. |

Hata enjeksiyonu yalnız `VIDSTOREX_ENABLE_TEST_HOOKS` ile derlenen kodda `VIDSTOREX_TEST_RECOVERY_IO_FAILURE` değişkenini okur. Yazma hatası içerik geçici dosyaya yazıldıktan sonra; flush ve close hataları ilgili gerçek çağrı sonrasında başarısız durum üreterek sınanır. Gerçek disk doldurulmadı. Bunlar gerçek donanım arızası deneyleri olarak sunulmuyor.

Aynı `safe_output.cpp`, test makrosu tanımlanmadan ayrı bir mini programda derlenip çalıştırıldı. Beş hata aşamasının ortam değişkenleri bu programda etkinleştirilmesine rağmen çıktı başarıyla yazıldı: üretim kodunda hook'ların etkisizliği doğrulandı. Bu ek smoke CTest sayısına dahil değildir; tam uygulamanın ayrı bir `BUILD_TESTS=OFF` derlemesi olarak sunulmuyor. Kanıt: `build-recovery-safety/production-smoke.log`.

Testlerin tamamı sentetik veriler kullanır. C++ fixture yalnız kendi başarıyla oluşturduğu dizini, çözülmüş mutlak yolun temp dizini altında kaldığını denetleyerek temizler. CLI fixture'ları her çalıştırmada yeni bir test alt dizini oluşturur; junction üzerinden recursive temizleme yapmaz. API/CLI hata senaryolarında kaynak video ve mevcut hedef SHA-256 karşılaştırılır. Doğrudan writer birim testlerinde kaynak video bulunmadığından yalnız sentinel ve geçici dosya durumu kontrol edilir.

Ara denemeler gizlenmedi: İlk test derlemesinde GoogleTest makrosu içindeki koşullu platform listesi derlenmedi; liste makro dışına taşınarak düzeltildi. CLI junction fixture'ı ilk çalışmada `cmd`'ye ileri eğik çizgili yol iletildiğinden başarısız oldu; Windows native yol dönüştürmesi eklenince aynı beklentilerle geçti. Üretim güvenlik beklentileri gevşetilmedi.

## 6. Tam test sonuçları

Derleme: Windows x64, NTFS, MSVC 19.44, C++23, Release; GUI ve testler açık. Ayrı klasör: `build-recovery-safety/compiled`. Son uygulama, DLL, CLI, Qt GUI ve test hedeflerinin derlemesi başarılı: `build-recovery-safety/build-final.log`.

| Test grubu | Toplam | PASS | FAIL | SKIPPED |
|---|---:|---:|---:|---:|
| Yeni kurtarma güvenliği regresyonları | 66 | 64 | 0 | 2 |
| Decoder: Codec, DCT, Stream | 51 | 51 | 0 | 0 |
| C API | 27 | 27 | 0 | 0 |
| Roundtrip, kriptografi, integrity | 47 | 47 | 0 | 0 |
| Fast Local, High Capacity, Video Set, Instant Recovery ve ilgili CLI | 90 | 90 | 0 | 0 |
| Qt GUI, model, dil, görsel kimlik ve sonuç kartı | 83 | 83 | 0 | 0 |
| Tam CTest | 591 | 589 | 0 | 2 |

Grup kanıtları: `build-recovery-safety/<grup>-verified-tests.log` ve `<grup>-verified-results.xml`; makinece okunabilir özet: `group-summary.json`. Grup satırları tam paketten ayrıca çalıştırılmış doğrulamalardır; tam paketin test sayısına tekrar eklenmezler. GUI testlerinde sahte yt-dlp ve yerel test sunucuları kullanılır; kişisel hesap veya arşiv kullanılmaz.

Tam CTest **161,09 saniyede** tamamlandı: **589 PASS, 0 FAIL, 2 SKIPPED**. Önceden mevcut 525 testin tamamı geçti. CTest'in `100% tests passed` mesajı atlanan testleri başarılı saydığımız anlamına gelmez: `Paths/RecoveryAlias.RejectsPhysicalSourceBeforeProgress/"symlink"` ve `.../"input_symlink"` çalıştırılamadı. Sonuç kaydı: `build-recovery-safety/full-tests.log` ve `full-results.xml`.

Tekrar çalıştırma:

```powershell
cmd /c build-recovery-safety\build.cmd
ctest --test-dir build-recovery-safety/compiled -R 'RecoveryOutputSafety|RecoverySuccess|RecoveryAlias|RecoveryIoFailure|RecoveryCancellation|RecoveryCorruption|CLI.LegacyRecoverySafety' --output-on-failure
ctest --test-dir build-recovery-safety/compiled --output-on-failure
```

## 7. Geriye dönük uyumluluk

Arşiv formatı, sürüm, FEC, CRC/XXHash, şifreleme algoritması, anahtar türetme ve eski video çözme biçimi değiştirilmedi. Legacy'de kaynak SHA-256 alanı eklenmedi. Resilient/High Capacity/Fast Local ve şifreli/şifresiz başarılı kurtarmalar test edildi. Video Set ve Instant Recovery regresyonları geçti; Task 01 kontrolleri korunuyor.

C API fonksiyon imzaları, enum değerleri ve `ms_decode_options_t` alanları değişmedi. Dahili C++ `Decoder` yazma metodu için ek varsayılanlı parametreler ve `SafeOutputFile` için yeni iç kullanım modu bulunuyor; bunlar kurulabilir C API ABI değişikliği değildir. Stream decoder da ortak güvenli writer'dan yararlanır. Mevcut progress callback'ine yayınlamadan önce son bir kontrol çağrısı eklenmiştir.

Başarı, legacy formatın mevcut kontrollerini geçme anlamındadır. Şifresiz veride geçerli checksum üreten farklı içerik, eksik terminal işareti ve farklı dosya kimliklerinin karışması için yeni bir garanti eklenmedi; bu son iki problem sonraki görevin kapsamındadır. Testlerin dışarıdan karşılaştırdığı SHA-256, formatın içinde kaynak hash'i bulunduğu anlamına gelmez.

## 8. Kalan riskler

- **TOCTOU:** Başlangıç ve son eşdeğerlik kontrolleri pencereyi daraltır. Dizin veya linkleri aynı anda değiştirebilen başka bir yerel süreç karşısında, dosya/dizin handle'larına bağlanan tam yarışsız erişim sağlanmadı. Exclusive-create, geçici adın kontrol/açma yarışını giderir; parent dizin değiştirme ve son kontrol/rename yarışlarını gidermez.
- **Windows yayınlama:** Hedef önceden silinmez veya ayrı bir konuma taşınmaz. Aynı dizindeki tek `MoveFileExW` işlemi kullanılır; paylaşım ihlalinde hedefin korunduğu NTFS üzerinde sınandı. Ağ dosya sistemleri, farklı diskler ve bütün sağlayıcı arızaları için aynı garanti iddia edilmez. `MOVEFILE_COPY_ALLOWED` kopyala/sil davranışını etkinleştirir; bu kodda kullanılmaz. [Microsoft MoveFileExW belgesi](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw).
- **ReplaceFile tercihi:** `ReplaceFileW`'nin bazı hata kodlarında hedefin adı değişmiş veya hedef kaldırılmış olabilir; bu nedenle altyapı kontrolsüz bir `ReplaceFileW` çağrısına dönüştürülmedi. [Microsoft ReplaceFileW hata durumları](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew).
- **Güç kesintisi:** `ofstream::flush()`/close ve `MOVEFILE_WRITE_THROUGH`, dosya ve dizin metadata'sı için tam bir güç kesintisi protokolü oluşturmaz. Ayrı `FlushFileBuffers`/fsync ve dizin kalıcılığı protokolü eklenmedi. Süreç çökmesinde geçici plaintext dosyası kalabilir; normal hata temizliği testi bu durumu kapsamaz.
- **Temizlik ve metadata:** Geçici dosya kaldırma destructor'da best-effort'tur; harici erişim engeli veya dosya sistemi arızasında artık dosya kalabilir. Yeniden adlandırma eski hedefin hardlink ilişkilerini ve tüm ACL/ADS metadata'sını koruma taahhüdü vermez. Diğer hardlinklerin eski içeriğinin korunduğu test edildi.
- **Platform doğrulaması:** Linux/macOS çalıştırılmadı. Windows dosya symlink'i testleri ayrıcalık eksikliğinden atlandı; junction testi bu iki dosya symlink testinin yerine PASS olarak sayılmadı.

## 9. Git diff özeti

Bu görev için başlangıçtaki kullanıcı/Task 01 değişiklikleri hariç sayım:

| Dosya | Eklenen | Silinen |
|---|---:|---:|
| `include/media_storage.h` | 8 | 0 |
| `src/decoder.cpp` | 21 | 16 |
| `src/decoder.h` | 4 | 1 |
| `src/fast_local_codec.cpp` | 10 | 0 |
| `src/media_storage_api.cpp` | 25 | 5 |
| `src/safe_output.cpp` | 68 | 4 |
| `src/safe_output.h` | 19 | 1 |
| `tests/CMakeLists.txt` — yalnız Task 02 | 16 | 0 |
| `tests/test_recovery_output_safety.cpp` | 466 | 0 |
| `tests/test_legacy_recovery_output.cmake` | 100 | 0 |
| Bu rapor | 170 | 0 |
| **Toplam — 11 dosya** | **907** | **27** |

Kod ve testler: **+737/-27**. Task 01'in `tests/CMakeLists.txt` içindeki mevcut +17 satırı bu görev sayımına dahil değildir. İzlenmeyen yeni test ve rapor dosyaları toplama dahildir. Göreve ait diff whitespace kontrolü geçti. Repo genelindeki kontrol, önceden var olan `README.md` satır 13 ve 29 trailing whitespace nedeniyle hata verir; kullanıcı değişikliği korunarak bırakıldı.

Commit, push, tag ve release oluşturulmadı. Build/test/smoke kanıtları Git tarafından yok sayılan `build-recovery-safety/` içindedir.

## 10. Son durum

**PARTIAL.** Kod düzeltmesi, iki veri kaybının düzeltme öncesi yeniden üretimi, çalıştırılabilen yeni 64 regresyon ve mevcut 525 test doğrulandı. Yanlış parola, algılanabilir bozuk veri, eksik chunk, iptal ve enjekte edilen I/O hatalarında mevcut hedef ve kaynak hash'leri korundu; yeni nihai çıktı oluşmadı. Aynı fiziksel kaynağa yazma mevcut ortamda oluşturulabilen yol eşdeğerleri için reddedildi. Tam pakette 589 PASS, 0 FAIL, 2 SKIPPED var. İki zorunlu dosya symlink senaryosu Windows ayrıcalığı nedeniyle dinamik olarak doğrulanamadığından koşulsuz PASS veya görevin bütün doğrulamalarının tamamlandığı iddia edilmiyor.

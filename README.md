# Reliquary

Türkçe | [English](README_EN.md)

Reliquary, dosyaları video karelerinin içine kodlayıp daha sonra tekrar birebir geri çıkarmayı denediğim bir arşivleme projesi. Fikir basit bir meraktan çıktı: Bir dosyayı görüntü verisine dönüştürürsem, onu normal bir video gibi saklayıp daha sonra geri alabilir miyim?

Buradaki önemli nokta videonun yalnızca açılması değil. Geri çıkardığımız dosyanın SHA-256 değeri orijinaliyle aynı değilse Reliquary işlemi başarılı kabul etmiyor.

> Reliquary deneysel bir araştırma ve mühendislik projesidir. Genel amaçlı bir yedekleme aracının veya güvenilir bir arşiv hizmetinin yerini tutmaz.

## Reliquary nedir?

Reliquary herhangi bir dosyayı küçük veri parçalarına ayırır, bu parçaları video karelerindeki görsel bloklara kodlar ve bir ya da daha fazla video üretir. Videolar yerelde saklanabilir veya normal videolar gibi YouTube'a yüklenebilir. Daha sonra aynı videolar indirildiğinde Reliquary içlerindeki veriyi okuyup kaynak dosyayı yeniden oluşturur.

Uygulamanın hem Qt 6 ile hazırlanmış masaüstü arayüzü hem de komut satırı aracı bulunuyor. Kodlama, kurtarma, şifreleme, onarım verisi, Video Set, ön kontrol ve test laboratuvarları aynı C++ altyapısını kullanıyor.

## Bu proje nasıl ortaya çıktı?

Bu proje aslında basit bir merakla başladı. YouTube kullanıcıların çok büyük miktarda video yüklemesine izin verdiği için aklıma şu soru geldi: Bir dosyayı video gibi gösterebilirsem, YouTube yeniden kodladıktan sonra içindeki veriyi tekrar okuyabilir miyim?

İlk prototiplerde tek bir ayarı deneyip “tamamdır” demedim. 8×8 ve 4×4 gibi farklı blok geometrileriyle, 1-bit ve 2-bit gibi farklı veri yoğunluklarıyla, farklı sinyal seviyeleriyle ve onarım oranlarıyla epey uğraştım. Bazı kombinasyonlar yerelde gayet iyi görünüyordu; fakat gerçek YouTube turundan sonra aynı güvenilirliği vermedi.

Bu yüzden Reliquary'de kapasite kadar geri alınabilirlik de önemli. Sonuçta çalışan profilleri yalnız teorik hesaba göre değil, gerçek yükleme–indirme–kurtarma turlarıyla ayırdım.

## Mantığı nasıl çalışıyor?

```text
DOSYA
  ↓
VERİ PARÇALARI
  ↓
VİDEO KARELERİNDEKİ BLOKLAR
  ↓
BİR VEYA DAHA FAZLA VİDEO
  ↓
YEREL DEPOLAMA / YOUTUBE
  ↓
İNDİRME VE TARAMA
  ↓
DOSYAYI YENİDEN OLUŞTURMA
  ↓
TAM SHA-256 DOĞRULAMASI
```

Bir dosyayı doğrudan YouTube'a atmıyoruz tabii; önce baytları paketlere, paketleri de video karelerindeki sinyal bloklarına dönüştürüyoruz. Kurtarma sırasında bu yol tersine işliyor. Eksik ya da bozuk parça varsa final dosya yayımlanmıyor.

## Neden video?

Video, hemen her yerde saklanabilen ve taşınabilen yaygın bir kapsayıcı. YouTube'un büyük miktarda video barındırabiliyor olması da bu fikri benim için ilginç hale getirdi.

Ama bunu “garantili sınırsız bulut depolama” olarak görmek doğru değil. YouTube videoları yeniden kodlayabilir; platform davranışı, politikaları ve limitleri zaman içinde değişebilir. Reliquary'nin tek gerçek başarı ölçütü, kurtarılan dosyanın tam SHA-256 değerinin kaynak dosyayla eşleşmesidir.

## Neleri denedim?

Küçük bloklar aynı 1080p kareye daha fazla veri sığdırıyor, fakat yeniden kodlamaya karşı daha hassas olabiliyor. Büyük bloklar daha az veri taşıyor, karşılığında daha geniş bir görsel sinyal bırakıyor.

- **4×4:** Aynı çözünürlükte daha yüksek kapasite ve daha kısa video hedefliyor.
- **8×8:** Daha düşük kapasite karşılığında daha muhafazakâr bir sinyal geometrisi kullanıyor.
- **1-bit / 2-bit:** Bir blokta taşınan veri yoğunluğunu değiştiriyor. Yoğunluk arttıkça yerel kapasite artabiliyor, fakat platform turundaki hata payı daralabiliyor.
- **Repair %5:** Verinin yanına sınırlı miktarda onarım paketi ekliyor. Daha yüksek oranlar daha fazla video süresi ve disk alanı kullanıyor.

Daha yoğun bazı kombinasyonlar gerçek YouTube turunda yeterince güvenilir çıkmadı. Bu nedenle README'de yalnız repository içinde kanıtı bulunan sonuçları kesin sayılarla veriyorum.

## Profiller

### High Capacity

Yeni bir Video Set oluştururken masaüstü arayüzünün varsayılan seçimi High Capacity'dir:

| Ayar | Değer |
|---|---:|
| Çözünürlük / kare hızı | 1920×1080 / 30 FPS |
| Blok geometrisi | 4×4 |
| Veri yoğunluğu | 1-bit |
| Sinyal | 1.0 |
| Onarım | %5 |
| Yapılandırma kimliği | `538F2B009FAB` |

4×4'ün güzel tarafı, aynı 1080p karede 8×8'e göre yaklaşık dört kat yararlı kapasite sunması. Bunun bedeli blokların küçülmesi olduğu için bu profili varsayımla değil gerçek YouTube turlarıyla test ettim.

- Tek videolu stres doğrulaması: **6/6 exact**
- Video Set doğrulaması: **4/4 parça, tam dosya SHA birebir**

Bu sonuçlar test edilen yükleme koşullarını doğrular; gelecekteki her yükleme için garanti vermez.

### Resilient

Resilient; 1920×1080 / 30 FPS, 8×8, 1-bit, 1.0 sinyal ve %5 onarım kullanır. Daha büyük bloklarla daha muhafazakâr bir seçenek sunar, fakat aynı dosya için daha uzun ve daha büyük videolar üretebilir.

Masaüstündeki yeni Video Set akışında High Capacity seçili gelir. Komut satırı, C API ve eski düşük seviyeli akışlarda geriye dönük uyumluluk için varsayılan profil Resilient olarak kalır.

## Video Set nedir?

Büyük bir dosyayı tek bir dev videoya dönüştürmek pek kullanışlı değil. Bu yüzden Video Set sistemini ekledim. Reliquary büyük dosyayı parçalara ayırıyor ve her parçayı ayrı, kendi içinde doğrulanan bir videoya dönüştürüyor.

Her videonun kimliği ve dosyadaki yeri videonun içine gömülüyor. Videoları indirip isimlerini tamamen değiştirseniz ve sıralarını karıştırsanız bile Reliquary metadata'yı okuyarak doğru parçaları bulabiliyor. İsim ya da playlist sırası kimlik olarak kullanılmıyor.

- Eksik parça varsa final dosya oluşturulmaz.
- Bozuk parça raporlanır ve kurtarma durur.
- Aynı parçanın birebir kopyaları güvenli biçimde ayırt edilir.
- Çelişen kopyalar kurtarmayı engeller.
- Tam dosya SHA-256 eşleşmeden “başarılı” sonucu verilmez.

İsim Reliquary oldu ama eski VidStoreX döneminde oluşturulmuş `VSXSET01` Video Set'leri bozmadım; okuma uyumluluğu korunuyor.

## YouTube ile nasıl kullanılır?

Normal akışta Google Cloud veya OAuth kurulumu gerekmiyor:

1. Reliquary ile videoları oluşturun.
2. Oluşan Video Set klasörünü açın.
3. Videoları YouTube'a elle yükleyin.
4. Gizlilik seçeneği olarak mümkünse **Liste Dışı** kullanın.
5. Tüm parçaları tek bir playlist'e koyun.
6. YouTube'un 1080p işleme sürecinin tamamlanmasını bekleyin.
7. Kurtarma zamanı geldiğinde playlist bağlantısını Reliquary'ye verin.
8. Parçaları indirip tarayın ve **Recover** işlemini başlatın.
9. Son tam dosya SHA-256 sonucunu kontrol edin.

Reliquary'nin üretim profilleri YouTube akışı için 1920×1080 ve 30 FPS kullanıyor. Test edilen sinyal geometrisi bu çözünürlüğe göre tanımlandığı için YouTube'un 1080p sürümünün hazır olmasını beklemek önemli.

## Dosyayı nasıl geri alırım?

Recover ekranına bir YouTube playlist bağlantısı yapıştırabilirsiniz. Reliquary, bulduğu `yt-dlp` aracını doğrudan çalıştırarak videoları indirir, gömülü bilgileri tarar ve tek bir eksiksiz set bulursa kurtarmaya hazırlar:

```text
PLAYLIST BAĞLANTISI
  ↓
YT-DLP İLE İNDİRME
  ↓
OTOMATİK TARAMA
  ↓
PARÇALARI TANIMA
  ↓
RECOVER
  ↓
SHA-256 DOĞRULAMASI
```

İndirme sonrası tarama otomatik yapılabilir. Tarama ile dosyayı gerçekten oluşturma ayrı adımlardır; normal manuel akışta Reliquary eksiksiz seti gösterir ve açıkça **Recover Original File** eylemini bekler. Instant Playlist Recovery ise indirme, tek set seçimi, kurtarma ve SHA doğrulamasını tek açık kullanıcı eylemi altında yürütür; birden fazla set veya sorunlu parça bulursa durur.

## Kurulum ve çalıştırma

Doğrulanan Windows yapısı Visual Studio Build Tools 2022, MSVC v143, Windows SDK, CMake 3.22+, Git ve vcpkg kullanıyor. Proje C++23 gerektiriyor; FFmpeg ve libsodium zorunlu, masaüstü arayüzü için Qt 6 gerekiyor.

```powershell
git clone --recurse-submodules https://github.com/burhanbty/VidStoreX.git Reliquary
cd Reliquary

cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_MANIFEST_FEATURES=gui `
  -DBUILD_TESTS=ON

cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

GUI hâlâ gerçek çıktı adıyla çalıştırılır; henüz `Reliquary.exe` adlı bir paket varmış gibi davranmıyorum:

```powershell
build\Release\media_storage_gui.exe
```

## CLI kullanımı

```powershell
# Tek videoya kodla ve geri al
build\Release\media_storage.exe encode input.rar output.mkv --reliability-profile high-capacity
build\Release\media_storage.exe decode output.mkv restored.rar

# Video Set planla, oluştur, tara ve kurtar
build\Release\media_storage.exe set-plan "D:\archive\large-file.rar" "D:\Reliquary Sets" --reliability-profile high-capacity
build\Release\media_storage.exe set-encode "D:\archive\large-file.rar" "D:\Reliquary Sets" --reliability-profile high-capacity --resume
build\Release\media_storage.exe set-inspect "D:\Reliquary Sets\my-set\returned"
build\Release\media_storage.exe set-recover "D:\Reliquary Sets\my-set\returned" "D:\Recovered" --resume
```

Şifreleme için encode komutuna `--encrypt --password "parola"`, decode komutuna `--password "parola"` eklenebilir. Paylaşılan terminal geçmişinde veya loglarda gerçek parolanızı bırakmayın.

Komut satırı seçenekleri, Fast Local biçimi, C API, Capacity Lab ve Test Lab için ayrıntılı teknik referans [İngilizce README'de](README_EN.md) bulunuyor.

## Gerçek testler

Repository'deki güncel Windows test yapısında **500/500 test geçiyor**. Kapsam; codec, şifreleme, onarım, SHA round-trip, GUI/CLI, Video Set, playlist kurtarma, YouTube Sync'in kontrollü testleri, yerelleştirme ve görsel davranış kontrollerini içeriyor.

Gerçek YouTube kanıtlarını birbirine karıştırmamak önemli:

- **High Capacity tek video:** Farklı yük boyutları ve yükleme oturumlarında 6/6 birebir kurtarma.
- **High Capacity Video Set:** 32 MiB kaynak, 4/4 geri dönen parça ve tam dosya SHA-256 eşleşmesi.

Yerel FFmpeg simülasyonları hızlı geri bildirim sağlar ama gerçek YouTube kanıtı sayılmaz.

## Experimental YouTube Sync

OAuth tabanlı YouTube Sync özelliği **Advanced → Experimental** altında bulunuyor. Google Cloud projesi, OAuth yapılandırması ve YouTube Data API kurulumu gerektirdiği için normal kullanıcı akışının merkezinde değil. Standart kullanım; videoları elle yüklemek, playlist bağlantısını yapıştırmak ve `yt-dlp` ile geri indirmektir.

Ayrıntılar: [YouTube Sync kurulumu](docs/YOUTUBE_SYNC_SETUP.md)

## Sınırlamalar

- YouTube güvence verilmiş bir arşiv hizmeti değildir; videoları yeniden kodlar ve davranışı değişebilir.
- Hiçbir profil gelecekteki platform işlemlerine karşı mutlak veri garantisi vermez.
- Resilient videolar kaynak dosyadan çok daha büyük olabilir.
- High Capacity daha kısa video hedefler ama sonuç içerik ve codec davranışına bağlıdır.
- Fast Local yalnız kayıpsız yerel FFV1/Matroska kullanımına yöneliktir; YouTube veya kayıplı dönüşüm veriyi bozabilir.
- Video Set v1, videolar arasında ortak parity içermez.
- Proje hâlâ deneysel ve Windows odaklıdır.

Önemli veriler için videoların yanında ayrı ve güvenilir bir yedek tutun. Kurtarma sonrasında mutlaka tam SHA-256 sonucunu kontrol edin.

## Neden Reliquary?

“Reliquary” kelimesi, değerli bir şeyi korumak için kullanılan özel bir muhafaza anlamına geliyor. Projenin yaptığı şeye bakınca bu isim bana oldukça oturdu.

## Kaynak proje ve lisans

Reliquary, Brandon Li'nin (PulseBeat02) geliştirdiği [yt-media-storage](https://github.com/PulseBeat02/yt-media-storage) projesini temel alır. Kaynak dosyalardaki mevcut yazarlık ve telif bildirimleri korunmuştur.

Proje **GPL-3.0-or-later** ile dağıtılır. Tam metin için [LICENSE.txt](LICENSE.txt) dosyasına bakabilirsiniz.

## Geliştirici

Projeyi ben, **Burhan Talha Yazıcı (BTY)**, geliştiriyorum.

[LinkedIn](https://www.linkedin.com/in/burhanbty)

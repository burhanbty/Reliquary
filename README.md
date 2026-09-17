# Reliquary

Türkçe | [English](https://github.com/burhanbty/Reliquary/blob/master/README_EN.md)

Reliquary(Kutsal emanetlerin saklandığı ve sergilendiği süslü kutu, sandık veya kap)

Günümüzün en büyük sorunlarından biri olan depolama sorunu için farklı bir yaklaşım ile yola çıktık. Bugün youtube twitch instagram ve daha niceleri gibi birçok platform bize sınırsız videolar ve fotoğraflar yükleyebilmemize olanak sağlıyor. Bizde acaba bu sınırsız video yüklememize izin veren platformlara bir şekilde videolarımızı depolamak haricinde farklı dosyalar depolayabilir miyiz diye düşündük

Reliquary, bir dosyayı videodaki her bir pikseli bir veriye dönüştürerek ve çeşitli sıkıştırma algoritmalarına karşı bir takım önlemler alarak video karelerine gömüp sonrasında tam olarak video formatına çevirilmiş dosyayı kayıpsız bir doğrulukla geri çıkarmayı deneyen bir arşivleme projesi.Youtube kullanıcıların devasa miktarda video yüklemesine izin veriyor ve kimse bize neden 1 saat uzunluğunda ve 100gb büyüklüğünde bir video yükledin demiyor bizde tam olarak bundan faydalanıyoruz.

Tek gerçek kriter şu: geri çıkan dosyanın SHA-256 değeri orijinaliyle birebir eşleşmiyorsa, iş bitmiş sayılmıyor.

> Not: Reliquary deneysel bir proje.  Program her ne kadar video paketlerinin sıkıştırma sırasındaki kayıplarına karşı birçok sağlama ve güvenlik katmanı içerse de Youtube platformunun bu algoritmaları ve süreçleri açıklamamasından dolayı güvenilir bir arşiv servisinin yerine geçmez. 

## Nasıl çalışıyor

Varsayılan High Capacity profilinde her veri biti kare üzerinde 4×4 piksellik bir blok ile temsil ediliyor ve her blok 1 bit taşıyor. Buradaki denge önemliydi: blokları küçülttükçe aynı kareye daha fazla veri sığdırabiliyoruz, fakat video sıkıştırması, yeniden ölçekleme ve platform kaynaklı bozulmalar karşısında hata riski artıyor. Bu yüzden yalnız kapasiteyi değil, okunabilirlik ile veri yoğunluğu arasındaki dengeyi hedefledik. Daha dayanıklı senaryolar için 8×8 blok kullanan Resilient profilini de koruyoruz.

Video üretmeden önce dosyayı bağımsız parçalara ayırıyor, her parçaya set kimliği, sıra bilgisi ve bütünlük verileri ekliyoruz. Ayrıca yaklaşık %5 repair verisi üreterek küçük kayıplarda veya hasarlı parçalarda kurtarma şansını artırıyoruz. Böylece sistem yalnızca “frame’leri tekrar okuyabiliyor mu?” sorusuna değil, “eksik veya bozulmuş veriyi ne kadar tolere edebilir?” sorusuna da cevap vermeye çalışıyor

Masaüstünde Qt 6 ile yazılmış bir arayüz ve ayrıca bir komut satırı aracı var. İkisi de aynı C++ altyapısını kullanıyor.

İlk denemelerde tek bir ayarla yetinmedim. 4×4 ve 8×8 blok boyutlarını, 1-bit / 2-bit veri yoğunluklarını, farklı sinyal seviyelerini ve onarım oranlarını denedim. Yerelde iyi görünen bazı kombinasyonlar YouTube'dan geri döndüğünde tutmadı — bu yüzden README'deki sayılar tahmini değil, gerçek yükleme-indirme turlarından geliyor.

## Profiller

Masaüstü arayüzünde yeni bir Video Set açtığınızda varsayılan **High Capacity**: 1920×1080, 30 FPS, 4×4 blok, 1-bit yoğunluk, %5 onarım (config id: `538F2B009FAB`). 4×4 aynı karede 8×8'e göre kabaca dört kat daha fazla veri taşıyor tek video stres testinde 6/6, Video Set testinde 4/4 parça tam SHA eşleşmesiyle geliyor. Bu yüzden f/p mantığına göre varsayılan olarak bunu seçtik.

**Resilient** ise 8×8 blok kullanıyor — daha büyük ve daha uzun videolar çıkarıyor ama daha toleranslı. Komut satırı ve C API'de geriye dönük uyumluluk için varsayılan hâlâ bu. 

## Video Set

Büyük bir dosyayı tek bir videoya sıkıştırmak pratik değil, o yüzden dosya parçalara bölünüp her parça kendi içinde doğrulanan ayrı bir videoya dönüşüyor. Video kimliği ve dosyadaki yeri videonun içine gömülü olduğu için dosya adlarını değiştirseniz veya sırasını karıştırsanız bile Reliquary doğru parçaları buluyor. Eksik parça varsa dosya oluşmuyor, bozuk parça bulunursa kurtarma duruyor.

## YouTube ile kullanım

Google Cloud ya da OAuth kurulumu gerekmiyor, akış şöyle:

1. Reliquary ile videoları oluşturun.
2. Video Set klasörünü açın, videoları YouTube'a elle yükleyin (mümkünse Liste Dışı olarak).
3. Hepsini tek bir playlist'e koyun ve YouTube'un 1080p işlemesini bitirmesini bekleyin.
4. Kurtarma zamanı geldiğinde playlist linkini Reliquary'ye verin, parçaları indirip **Recover** işlemini başlatın.
5. Son olarak SHA-256 sonucunu kontrol edin.

OAuth tabanlı otomatik senkronizasyon (Experimental YouTube Sync) da var ama Google Cloud/API kurulumu gerektirdiği için standart akışın parçası değil, `Advanced → Experimental` altında.

## Kurulum

Windows x64 için hazır installer ve portable ZIP var (`Reliquary-Setup-v1.5.0-x64.exe`, `Reliquary-v1.5.0-portable-x64.zip`), bütünlük `SHA256SUMS.txt` ile doğrulanabilir. Paketler imzasız, SmartScreen uyarı verebilir.

Kaynaktan derlemek için Visual Studio Build Tools 2022, MSVC v143, CMake 3.22+, Git ve vcpkg gerekiyor (proje C++23, FFmpeg + libsodium şart, GUI için Qt 6):

```
git clone --recurse-submodules https://github.com/burhanbty/Reliquary.git
cd Reliquary

cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_MANIFEST_FEATURES=gui `
  -DBUILD_TESTS=ON

cmake --build build --config Release --parallel
```

CLI örneği:

```
build\Release\media_storage.exe encode input.rar output.mkv --reliability-profile high-capacity
build\Release\media_storage.exe decode output.mkv restored.rar
```

Şifreleme istiyorsanız `--encrypt --password "parola"` (decode'da `--password`) eklenebilir.

## Sınırlamalar

YouTube garanti verilmiş bir depolama servisi değil; videoları yeniden kodluyor ve davranışı zamanla değişebilir. Hiçbir profil gelecekteki platform değişikliklerine karşı kesin garanti vermiyor. Resilient videolar kaynak dosyadan epey büyük olabilir, Video Set v1'de parçalar arası ortak parity yok, ve proje şu an Windows odaklı. Kısacası: önemli dosyalar için Reliquary'nin yanında ayrı bir yedek bulundurun, kurtarma sonrası SHA-256'yı kontrol etmeyi atlamayın.

## Kaynak ve lisans

Reliquary, Brandon Li'nin (PulseBeat02) [yt-media-storage](https://github.com/PulseBeat02/yt-media-storage) projesi üzerine kuruldu, orijinal telif bildirimleri korunuyor. Proje GPL-3.0-or-later ile dağıtılıyor, detay için [LICENSE.txt](https://github.com/burhanbty/Reliquary/blob/master/LICENSE.txt).

## Geliştirici

**Burhan Talha Yazıcı (BTY)** — [LinkedIn](https://www.linkedin.com/in/burhanbty)

---
Komut satırı seçenekleri, C API, Fast Local format ve Capacity/Test Lab gibi teknik detaylar için [İngilizce README](https://github.com/burhanbty/Reliquary/blob/master/README_EN.md)'ye bakabilirsiniz.

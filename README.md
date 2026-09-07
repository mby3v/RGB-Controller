# RGB-Controller
NodeMCU (ESP8266) ve 2x PCA9685 ile 10 Kanallı Web Kontrollü RGB LED Sürücü

# 10 Kanallı RGB LED Kontrol Paneli (ESP8266 + 2x PCA9685)

NodeMCU (ESP8266), 2 adet PCA9685 PWM sürücü ve ULN2003 transistör dizilimleri kullanılarak geliştirilmiş, web arayüzü üzerinden kontrol edilebilen 10 kanallı yüksek güçlü RGB LED kontrol sistemi.

 Bütçe Dostu & Yüksek Fiyat/Performans Mimari
> Bu proje; yüksek maliyetli adreslenebilir LED sistemleri veya pahalı endüstriyel sürücüler yerine piyasada en kolay bulunan, son derece uygun fiyatlı bileşenlerle (**NodeMCU, PCA9685, ULN2003 ve standart 12V RGB şerit LED**) maksimum fiyat/performans oranı yakalamak amacıyla tasarlanmıştır.

Neden Bu Mimari?

* **Düşük Maliyetli LED Şeritler:** Metre fiyatı adreslenebilir dijital LED'lere göre çok daha ucuz olan standart 12V (Ortak Anot) analog şerit LED'ler kullanılır.
* **Ekonomik Güç Anahtarlama:** Pahalı harici MOSFET kartları yerine, adet fiyatı oldukça düşük olan çok kanallı **ULN2003** transistör dizilimleriyle 12V hatlar güvenle sürülür.
* **Maksimum Donanım Verimi:** NodeMCU'nun kısıtlı GPIO pinlerini harcamak yerine, I2C hattı üzerinden bağlanan iki adet **PCA9685** ile 30 bağımsız PWM kanalı donanımsal 12-bit hassasiyetle kontrol edilir.
* **Özel PCB Gerektirmez:** Fabrikasyon bir baskı devre kartı (PCB) tasarlamaya veya sipariş edip beklemeye gerek yoktur. DIP kılıflı ULN2003 entegreleri; küçük parçalara kesilmiş standart delikli plaketlere (pertinaks) lehimlenerek veya doğrudan havada lehimleme yöntemiyle kurulabilir. Ev ortamında, temel bir havya ile herkesin kolayca uygulayabileceği bir yapıya sahiptir.

Özellikler

* **Web Arayüzü:** Dahili web sunucu üzerinden mobil/masaüstü uyumlu kontrol paneli.
* **18 Farklı Çalışma Modu:** Sabit renk, tekli LED kontrolü, nefes, meteor, gökkuşağı, alev, kara şimşek vb.
* **12-bit Gamma Düzeltmesi:** Düşük parlaklık seviyelerinde dahi basamaksız, göze doğrusal geçişler.
* **Optimize I2C Trafiği:** Framebuffer ve gölge tampon mimarisiyle sadece değişen PWM kanalları I2C hattına basılır.
* **OTA Desteği:** Tarayıcı üzerinden kablosuz bellenim güncelleme (`/update`).
* **Açılış Animasyonu:** Sistem açıldığında sıcak beyaz (#FFD9A8) tonunda 4 fazlı akıcı başlangıç sekansı.

Donanım Gereksinimleri

* 1x NodeMCU V3 (ESP8266)
* 2x PCA9685 16-Kanal 12-bit PWM Modülü
* 5x ULN2003A Darlington Transistör Dizisi
* 10x Ortak Anot (Common Anode) 12V RGB LED Şerit / Bar
* 1x 12V Güç Kaynağı (LED'ler için)
* 1x 5V Güç Kaynağı (NodeMCU ve PCA9685 lojik beslemesi için)

> **Önemli Donanım Notları:**
> 1. İkinci PCA9685 modülünün adresini **0x41** yapmak için kart üzerindeki **A0** adres lehim köprüsü birleştirilmelidir (varsayılan: 0x40).
> 2. 5V ve 12V güç kaynaklarının **GND (eksi)** hatları birbirine bağlanarak ortak şase yapılmalıdır.

Bağlantı Şeması

![Devre Şeması](schema.jpg)

Gerekli Arduino Kütüphaneleri

Arduino IDE > Kütüphane Yöneticisi üzerinden kurulması gerekenler:
* `Adafruit PWM Servo Driver Library`
* `WiFiManager` (tzapu)
* `ESP8266WiFi` & `ESP8266WebServer` (ESP8266 board paketiyle gelir)

Kurulum

1. `RGB_Controller/RGB_Controller.ino` dosyasını Arduino IDE ile açın.
2. Kart olarak **NodeMCU 1.0 (ESP-12E Module)** seçin.
3. Kodu karta yükleyin.
4. Cihaz açıldığında oluşturacağı `NodeMCU-LED-Kontrol` isimli Wi-Fi ağına bağlanarak ev ağınızın bilgilerini girin.
5. Tarayıcınızdan `http://ledpanel.local` veya seri monitörde yazan IP adresine (`192.168.1.202`) gidin.

Görseller ve Animasyonlar

### Proje Görselleri
![Fotoğraf 1](foto1.jpg)
![Fotoğraf 2](foto2.jpg)

### Efekt Demoları
![Animasyon 1](gif1.gif)
![Animasyon 2](gif2.gif)

### Web Arayüzü
![Arayüz 1](foto3.png)
![Arayüz 2](foto4.png)

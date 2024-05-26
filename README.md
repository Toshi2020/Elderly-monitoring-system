# Elderly monitoring system  
# 高齢者見守りシステム  
  
**●概要**  
・実家で一人暮らしをしている老いた母をゆるく見守るためのシステムを作りました。  
・モーションセンサーによる動きの検知が長時間途絶えたり、室温が高くなり過ぎたり、部屋の明かりがずっと点灯/消灯しているときに自動でメールが送られてきます。カメラによる監視などは行いません。  
  
**●システム構成**  
![システム構成L](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/282dd7f7-c3b9-425b-b73d-95ade0f36a08)


・廊下と勝手口にモーションセンサーを持つ子機を設置、リビングにモーションセンサー、光センサー(CdS)を持つ親機を設置します。  
・親機はリビングに置かれた温度計とBLE通信により室温を取得します。  
・子機はWEMOS D1 mini(ESP8266)、親機はESP32C3 Super Mini + ESP-01(ESP8266)を使っています。  
![取り付け風景](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/b5d39023-37d9-434a-815e-dce9cba68c26)

  
**●ハードウェア**  
・子機：WEMOS D1 miniにモーションセンサAM312を接続しただけのものです。動作確認用にLEDを追加しましたが、3Dプリントした白色のケースが光を透過することがわかったので、今思えばオンボードLEDを使えばよかったと思います。  
![回路図子機](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/766df74d-7e1b-4f50-bf85-1a2d851b1e76)

  
・親機：ESP32C3 Super MiniとESP-01にCdSとモーションセンサAM312を接続しています。オンボードLEDが暗いので、LEDを追加しました。(子機とLEDの論理が逆になってしまった理由は思い出せません)  
![回路図親機](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/a9362f4a-4822-4a06-a15d-e1cabc6c0cb6)
![親機外観](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/1a8d9789-afa3-48db-aa60-14e3edb7bdd5)


  
・LEDは親機子機ともにWiFi接続前は点滅、モーションセンサ出力がHの間は点灯、さらに親機に関しては子機が検知したら2回点滅します。  
・電源は子機親機共にUSB出力のACアダプタを使っています。よってACコンセントのある場所に設置することになります。  
  
**●使用したモーションセンサについて**  
・入手可能なセンサとしては小型のAM312と、高感度のHC-SR501が出回っています。当初リビング用の親機は検知距離を稼げるようにHC-SR501を使用したのですが、WiFiの電波で誤検知が多発してしまい使い物にならなりませんでした。HC-SR501を使うならきちんとシールドするか、距離を離してレイアウトする必要がありそうです。  
・ちなみにESPからどれほど強い電波が出ているのかを  
https://minkara.carview.co.jp/userid/3336538/blog/44912588/  
の最後に書いた簡易型の電波検出器を当ててみたのですが、針はピクリとも動きませんでした。

![モーションセンサ](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/fb5ee7ac-5cc2-450b-b7fa-cd1aaa2735fd)


  
**●親機の仕様の変遷**  
1X(初号機)：WEMOS D1 mini(ESP8266)+モーションセンサーのみ。  
2X：明るさ検知のためにCdS追加。  
3X：ESP8266とESP32C3 SuperMini両方に対応。温度センサーLM35追加。  
4X(現在)：ESP8266ではメモリ不足となりESP32C3 SuperMini専用とする。室温はBLEによる外部温度センサーから得るように変更。ESPNOW通信専用のESP-01(ESP8266)のサブ基板を追加。  
  
**●基本動作**  
・子機はモーションセンサーが検知したことをESP-NOWで親機に送信します。  
・親機は子機から受信したらその子機に対してACK(アクノリッジ)を送信します。  
・子機はACKを受けるまで規定回数内で再送します。  
・親機のESP-NOW通信はサブボードのESP-01(ESP8266)で行い、受信結果をメインボードのESP32C3へシリアルで送信します。  
・親機は各種異常を検知したら設定したメールアドレス宛にメールを送信します。  
  
**●その他の動作**  
  
**■親機のみの機能として**  
・日付が変わった時(各センサが最も検知しないであろう時間帯)に以下の動作を行います。  
　・動作状況の1時間ごとの統計データをGoogleスプレッドシートにアップロードします。(センサの検知回数、在宅かどうか、室温、部屋の明るさ)  
　・各種設定パラメータをGoogleスプレッドシートから読み取ります。  
　・GoogleスプレッドシートにOTA書き換え要求が記述されていたら、Googleドライブに置かれているバイナリーファイルを使ってプログラムの書き換え(遠隔OTAアップデート)を行います。  
・フェールセーフとして、ヒープサイズが起動時よりも規定値以下になったらホットスタートします。これは当初親機がESP8266だったので、連続稼働中にヒープ領域が不足してWebアクセスが不能になるのを回避する策でした。今のESP32ならばメモリに余裕があるとは思いますが念のため残してあります。  
  
**■親機＆子機共通の機能として**  
・宅内WiFiからプログラムの書き換え(WiFi OTAアップデート)が可能です。  
・WiFiシリアルまたはUSBシリアルで動作状況の表示と設定パラメータの読み書きが可能です。  
・自宅のWiFi環境と実家のWiFi環境を自動判別するので再ビルドしなくてもOKです。これにより自宅WiFi環境で動作確認を済ませたらそのまま実家のWiFi環境で使うことができます。(各SSIDをユニークな物にしておく必要があります)  
  
**●メールの種類と送信する条件 ()内の値は設定パラメータなので変更可能**  
・在宅中で(4)時間以上どのモーションセンサーにも検知がなかったか、または起床時刻を1時間過ぎてもどのモーションセンサーにも検知がなければ、不検知のメールを送信します。  
・起床時刻は(6)時に、就寝時刻は(22)時と設定してあります。  
・就寝時刻から起床時刻までは就寝中とみなして不検知時間をカウントしません。  
・不検知のメールを送信後、いずれかのモーションセンサが再検知したら再検知のメールを送信します。  
・リビングの温度が(31)℃以上でリビングに(30)分以上在室していると判定した時に熱中症注意メールを送信します。  
・リビングの温度が50℃以上になったら火災感知のメールを送信します。  
・就寝中にリビングが明るい時間をカウントし、(12)時間以上になったら連続点灯のお知らせメールを送信します。  
・就寝中でない時間にリビングが暗い時間をカウントし、(12)時間以上になったら連続消灯のお知らせメールを送信します。(昼間なのにカーテンが閉まったままか寝る時間前なのに照明がオフのまま)→リビングが遮光カーテンではないのでうまくいかないかも(カーテンが閉まっている日中でも明るいと判定されてしまう)  
・冬の期間、リビングのモーションセンサーが夜中に反応することがありました。おそらく暖房を消した後で部屋の温度が急激に下がるためと思うのですが、CdSにより部屋が暗い夜間はモーションセンサーを無視する設定を可能としています。(GoogleSpreadSheetで設定)  
  
**●在宅判断の手法**  
・10分ごとにスマホにpingを送り、3回応答がなければ外出と判断します。スマホ側ではIP固定に設定しておきます。3回待つのはスマホがたまたまスリープしていることを考慮したためです。  
・ただしスマホが3日以上検知できなかったら故障または電源切れとみなして在宅扱いとしています。(3日以上の旅行はしないだろうという判断)  
  
**●リビング在室判断**  
・リビングのモーションセンサが検知したら在室、10分間検知がなければ退室と判断します。  
  
**●親機のホットスタートとコールドスタート**  
・再起動しても残しておきたいデータはリブート前にEEPROMに書き込んでおきます(ホットスタート)  
・ホットスタートであること自体もEEPROMに書き込むので、今回の起動がホットスタートであれば変数にEEPROMの内容をコピーし、コールドスタートの場合は変数をクリアします。  
  
**●今回アップロードしたソフトウェア**  
・mimamori4/　　・・親機のESP32C3のソースコードです  
・mimamori4_sensor/　　・・子機のWEMOS D1のソースコードです  
・mimamori4ESPNOW/　　・・親機のサブボードESP-01のソースコードです  
・GoogleSheetスクリプト.txt　　・・Googleスプレッドシートのスクリプトです  
  
・作りながら仕様を追加していったので特に親機に関しては美しいコードではないですが、部分的にでも参考になるようならばうれしく思います。  
・当初ESP8266でヒープを稼ぐために文字列リテラルをROMに置くためのFマクロを多用しています。ESP32なら不要でしょうがそのまま残してあります。  
  
**★★★★★★備忘録＆参考リンク(以下は開発中のメモに加筆した物です)★★★★★★**  
  
**●ESPからG-mailでメールを送信するために**  
・見守りセンサ用のGoogleアカウントを新たに作成。  
・アカウントの2段階認証を有効にしてアプリパスワードを取得。  
・ソースのAUTHOR_EMAILとAUTHOR_PASSWORDに設定する。  
・参考URL  
>ESP32でメール送信  
https://randol-news.net/mon/202309-1.html  

>ESP-Mail-Client  
https://github.com/mobizt/ESP-Mail-Client  

>ESP32 Send Emails using an SMTP Server  
https://randomnerdtutorials.com/esp32-send-email-smtp-server-arduino-ide/  
  
**●ESPからGoogleスプレッドシートを読み書きするために**  
・Googleドライブにセンサとのやり取りをするスプレッドシートを作成。例えば"見守りセンサー"  
・スプレッドシートにリビング/廊下/勝手口/スマホ/温度/明度/設定の7枚のシートを作成。  
・設定以外のシートの1行目に"年月日＼時刻/0,1,,,23"を記述。表示/固定/1行でスクロール範囲を2行目以降とする。  
![スプレッドシート1](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/ca94c7fa-86e1-4b27-8458-774e95c9b5b3)




・設定のシートA列に以下の項目を、B列には設定値を書き込む。親機では項目名をサーチしておらず場所を決め打ちしているので並びを変えたらESP側のプログラムを変更する必要がある。  
検出間隔[時間]  
就寝時刻[時]  
起床時刻[時]  
メール0  
メール1  
メール2  
初期化＆再起動要求  
CdS利用  
スマホ存在利用  
熱中症警報温度[℃]  
熱中症警報時間[分]  
連続点灯注意時間[時]  
連続消灯注意時間[時]  
温度オフセット[℃]  
明るさ判定A/D値  
OTAファイルID  
OTAアップデート要求  
![スプレッドシート2](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/4305da0a-3668-4c9e-bd7f-c07544f63dda)




  
・スプレッドシートをオープンし、拡張機能/Apps Scriptで"GoogleSheetスクリプト.txt"の中身を貼り付ける。  
・参考URL  
>ESP8266/ESP32でGoogleSpreadSheetのセルを読む  
https://qiita.com/DeepSpawn/items/f1ad2abaf18f1a419ae6  

>ESP8266/ESP32でGoogleSpreadSheetのセルを更新する  
https://qiita.com/DeepSpawn/items/2799a894f80a79b40974  

>スプレッドシートを開いたら自動的に最終行に移動してみる  
https://for-dummies.net/gas-noobs/move-to-the-last-data-cell-on-open-automatically/  

>[GAS]スプレッドシートの最終行にセルを移動  
https://note.com/kawamura_/n/nb1865dfb3c77  
  
・スクリプト1行目のスプレッドシートIDは、ドライブでファイル"見守りセンサー"を右クリックし、"共有/リンクをコピー"した  
"https://docs.google.com/spreadsheets/d/～/edit?usp=drive_link"
の～部分  
・ソースコードのDeployUrl0/1はスプレッドシートをオープンし、デプロイ/新しいデプロイ/で、"次のユーザーとして実行"は"自分"、"アクセスできるユーザー"を"全員"としてデプロイを作成し、ウェブアプリのURLの"https://script.google.com/macros/s/～/exec"
をまるごとコピペ。  
・Googleスプレッドシートアクセス時には要求したURLがリダイレクトされるので、当初HTTPSRedirectライブラリを使ったがESP8266ではヒープ不足で動作しないかクラッシュしてしまう(おそらく再起呼び出しを行ってる部分)。そこでリダイレクト処理は以下のリンクを参考に自前で行う事とした。  
・参考URL  
>ESP32 オンライン OTAを実装してみた (Googleドライブ使用)  
https://note.com/rcat999/n/n179e5b71ebc9  

・WebアクセスにはDoGetとDoPostの2種類のメソッドがあるが、URLとパラメータを同時に送るDoGetを使っている。DoPostだとESP側のペイロード作成が視覚的にわかりづらかったので。  
・URLで日本語をパラメータとして送るためエンコード処理が必要。  
  
**●URLエンコード**  
・参考URL  
>URLエンコード  
https://github.com/plageoj/urlencode/blob/master/src/UrlEncode.cpp  
  
**●開発用UI**  
・センサーユニットとはUSB接続時は115200bpsのシリアル入出力だが、これを使うとシリアル開始時にESPにリセットがかかってしまう。  
・WiFiシリアルを使えば動作を継続したまま入出力ができる。WindowsならRLogin、AndroidならSerial WiFi Terminalアプリが使いやすい。送受信の行末はCR+LFに設定。ホスト名は親機ならmimamori32.local、親機のサブボードならmimamoriESPNOW.local、子機ならmimamori_1.localやmimamori_2.localと接続。TCPポートはESP側のソフト側で設定するが、今は54321としている。  
・ESP側ではSerial.printとClient.printを同時に行うマクロを用意した。  
![WiFiシリアル](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/461e4f80-eeaf-42f6-aefa-2a5e10b10ae5)

  
・DNSキャッシュのクリアはWindowsならcmdプロンプトで"ipconfig /flushdns"、Androidは端末再起動。  
・同名のホストを2つ起動するとホスト名の末尾に"_2"が付けられる。(WiFiルーター側仕様？)  
・実際に割り振られたホスト名とIPアドレスは、ArduinoIDEで"ツール/シリアルポート"で確認できる。  
・親機/子機ともに"?↓"で今の設定と設定コマンド一覧が表示される。(↓はエンター)  
・子機では#1(廊下用)で"-b1↓"、#2(勝手口用)で"-b2↓"でセンサー番号を設定しておく。  
・親機ではいくつかの設定項目はスプレッドシートでも設定できる(ただしスプレッドシートからの反映は日付が変わった時)。その他開発用のテスト機能も実行できる。  
・メールライブラリなど、内部からUSBシリアルに直接送っている情報はWiFiシリアルでは表示されない。  
  
**●ESP-NOW**  
・送受信それぞれでWiFiチャンネルを同一にしておかないと通信できない。  
・よって子機もWiFiに接続しておく。これによりWiFiシリアルによる情報の入出力やWiFiによるOTAアップデートが可能。  
・子機(送信側)がESP8266で親機(受信側)がESP32だとブロードキャスト以外受けられないようだ。(親機でWiFi.beginを行うとESP32のMACで受信できなくなる。AP用のMACに送ってもダメ。もちろん同一チャンネル。ESP32同士ならWiFi.beginを行っても受信できるのだが)  
・子機はモーションセンサ検出時に子機番号(1or2)とパケットカウンタ+1の2バイトを送信する。  
・子機は送信後、親機からのACKを受信したら終了、ACKが来なければ30回リトライ。リトライ中はパケットカウンタの値は変化しない。  
・親機がESP8266ならACKは1バイト、ESP32の場合はACKを2バイト送信する仕様とした。  
・子機は初回はブロードキャストアドレスに送信し、ACKが1バイトなら次回から親機のアドレスに送信する。ACKが2バイトなら次回以降もブロードキャストアドレスに送信。  
・その後、親機でBLEを使い始めたところ、BLEのスキャン中はWiFiが使えない(ESP-NOWの受信ができない)ことが分かった。そこで子機とのESP-NOWによる通信はESP-01(ESP8266)によるサブボードで行い、親機へは子機から受信したことをシリアルで送るようにした。  
・ESP-01のシリアル出力は通常のデバッグ用文字出力と共用。センシング時には">SENS=1"または">SENS=2"の文字列を送り、ESP32側で受信した文字列の一致によりセンスしたことを判断する。  
・ESP8266でのESP-NOW送信ではピアリストへの登録は不要、ESP32では必要。  
・参考URL  
>ESP8266とESP32 温度測定にESP-NOWを使ってみる  
https://okiraku-camera.tokyo/blog/?p=7167  

>ESP-NOWを使ってみた【ESP32】  
https://it-evo.jp/blog/blog-1397/  

>ESPNOWの送受信関数群とサンプルです  
https://qiita.com/DeepSpawn/items/06b378eeef3b4a4ff0b0  

**●BLEによる外部温度計(シャオミLYWSD03MMC)との通信**  
・当初リビングの温度検知としてセンサーLM35やMCP9700Aを組み込んでみたが、設置場所が部屋の上方であるのと本体の発熱の影響を受けて正確な室温が得られなかった。そこで外部温度計からBLEで温度を取得することにした。  
・参考URL  
>600円の温湿度計(LYWSD03MMC)とESP32でIoT  
https://momijimomimomi.com/makers/LYWSD03MMC_Thermo-Hygrometer_BLE_ESP32_Ambient.html  

>【Home AssistantでDIY Smart Home】Xiaomi四角温湿度計を改造！  
https://maky-ba.hatenablog.com/entry/2021/06/18/215016  

>格安BLE温湿度計のデータをESP32で取得してみた  
https://qiita.com/kobayuta/items/947b69af6360d70d7f26  

・BLEを使う時はWiFiのスリープを禁止するとクラッシュする。(STAモードの場合WiFiスリープを禁止しないとESP-NOWで受信がドロップする)  
・LYWSD03MMCは内蔵のCR2032で1年間稼働するとのことだが、外付けバッテリーでさらに長期間動作できるようにしてみた。  
	https://www.thingiverse.com/thing:6513933  
![温度計](https://github.com/Toshi2020/Elderly-monitoring-system/assets/81674805/1c8b3ec6-71e1-49f3-a6eb-a31e775af325)

  
**●ESP32C3の内蔵温度センサーによる外気温の測定**  
・外部温度センサーの電池切れ時の対応や、火災に関しては部屋の上方に設置したセンサの方が素早く感知できると思われるのでESP32C3の内部温度センサーでも外気温を計測する。  
・内部温度センサーはほぼジャンクション温度Tjを計測しているとするなら、  
　周辺温度Ta = Tj - Rth・P   ここでRth=熱抵抗、P=消費電力  
　P=平均すれば一定、Rthは周辺空気の自然対流を考慮するとTjとTaの温度差に影響を受けると考えられる。  
　完成したユニットを3Dプリンタのベッド上で容器を被せて加熱して実測したところ、  
　オフセット分RthP = mapf(Tj, 60.0, 90.0, 55, 61) となった。  
・微調整できるよう一律のオフセット補正も残してある。(シリアルコマンドまたはスプレッドシート)  
・参考URL  
>素子温度の計算方法  
https://www.rohm.co.jp/electronics-basics/transistors/tr_what7  

>Temperature Sensor  
https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32c3/api-reference/peripherals/temp_sensor.html  

**●WiFiによるOTA**  
・参考URL  
>ESP32/ESP8266でネットワーク経由でスケッチを更新する  
https://programresource.net/2020/02/21/2916.html  

>OTA Updates  
https://arduino-esp8266.readthedocs.io/en/latest/ota_updates/readme.html  

・ArduinoIDEより書き込み先をWiFiホストとしてOTAを行う。パスワードはWiFiの物をそのまま使っている。  

**●GoogleDriveに置いたバイナリファイルによる遠隔OTA**  
・参考URL  
>ESP32 オンライン OTAを実装してみた (Googleドライブ使用)  
https://note.com/rcat999/n/n179e5b71ebc9  

・拡張子.binのバイナリファイルはArduino IDEで「スケッチ/コンパイルしたバイナリを出力」でinoファイルのフォルダに出力される。  
・バイナリファイルをGoogleドライブにアップロードし、右クリックし"共有/リンクをコピー"で得られた  
"https://drive.google.com/file/d/～/view?usp=drive_link"
の～部分をGoogleSpreadSheetの設定シートの"OTAファイルID"にコピーしておく。  
・設定シートの"OTAアップデート要求"を1にすると日付が変わった時にアップデートが行われる。  
・Googleドライブではファイルをアップデートしても置き換えを選択すればファイルIDが変わらないことが分かったので、IDはプログラム側に埋め込んでおいた方が良かったかもしれない。  
  
**●開発環境(Arduino IDE)**  
・環境設定で追加のボードマネージャ―を設定  
http://arduino.esp8266.com/stable/package_esp8266com_index.json  
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json  
・ESP8266 BoardsとESP32 Arduinoを組み込む。  
**★ツール設定**  
・ESP8266(WEMOS D1 mini)  
　　ボード:LOLIN(WEMOS)D1 R2 & mini  
　　Flash Size:4MB(FD:2MB OTA:～1019KB)  
・ESP-01  
　　ボード:Generic ESP8266 Module  
　　Flash Size:1MB(FD:64KB OTA:～470KB)  
・ESP32C3(Super Mini)  
　　ボード:ESP32C3 Dev Module  
　　USB CDC On Boot:"Enabled"を選択(USBシリアル出力を有効にするため)  
　　CPU Frequency:"80MHz(WiFi)"を選択(発熱を少しでも減少させるため)  
　　Partition Scheme:"MinimalSPIFFS(1.9MB APP with OTA/190kB SPIFFS))"を選択  
　　(プログラムサイズが大きいのでDefaultでは入らないため)  
  
**★インストールしたライブラリ**  
・Mail  
　　ESP Mail Client by Mobizt 最新ver 3.4.16  
・ping  
　　ESPping 最新ver 1.0.4  
  
**★コンパイル時間の短縮**  
・ArduinoIDEではでそのままでは毎回全ライブラリのリビルドが行われ、特にESP32ではコンパイルに時間がかかり過ぎるので以下の処理を行う。  
・C:\ユーザー\ユーザー名\AppData\Local\Arduino15\preferences.txtを同じ場所にコピーして、  
　　preferences_esp-01.txt  
　　preferences_esp8266.txt  
　　preferences_esp32.txt  
にリネーム。  
・それぞれのファイルに以下の行を追加。(build.pathがすでに存在していたら編集)  
　　build.path=C:\Users\ユーザー名\AppData\Local\Temp\ArduinoBuild\esp-01  
　　build.path=C:\Users\ユーザー名\AppData\Local\Temp\ArduinoBuild\esp8266  
　　build.path=C:\Users\ユーザー名\AppData\Local\Temp\ArduinoBuild\esp32  
・デスクトップにarduino.exeのショートカットを3つ作成し、それぞれ名前をESP-01, ESP8266, ESP32と変更し、リンク先として先ほどのファイルを指定  
　　D:\Programs\Arduino1.8.19\arduino.exe --preferences-file C:\Users\ユーザー名\AppData\Local\Arduino15\preferences_XXX.txt  
・参考URL  
>Arduino IDE でESP32のコンパイル時間を短縮する方法  
	https://qiita.com/njm2360/items/c8a15047cde43617f6ce  
  
>ArduinoIDEの便利技【技その１】起動時に設定を変える  
	https://raspberrypi.mongonta.com/tips-arduinoide/  

・↑ではccacheを使うやり方が紹介されていて試してみたが、現在のCPUやdiskアクセスが充分速いためかあまり効果が感じられなかった。  
・ESP32のビルドで、たまに停止してしまうことがありprocexpで一連のJAVAのプロセスをdelして再コンパイルする必要があった。ソースの大幅改変後の初回ビルドで発生しやすい。  

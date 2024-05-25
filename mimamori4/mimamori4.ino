/*****************************************************************************
*
*	mimamori4.ino -- 見守りセンサー 4X(親機) for ESP32C3
*
*	・各種異常を検知したらメールで知らせる
*	・動作状況の統計データをGoogleSpreadSheetにアップロードする
*	・設定パラメータをGoogleSpreadSheetから読み取る
*	・GoogleDriveのバイナリーファイルを使って遠隔でOTAアップデート可能
*	・宅内WiFiからOTAアップデート可能
*	・宅内WiFiシリアルで動作状況の表示とパラメータの変更可能
*	・外部温度計よりBLEで室温を取得
*	・ESP-NOWはサブ基板経由で受け取る
*
*	・ESP32C3ではArduino IDEの[ツール]で
*	  USB CDC On Boot:"Enabled"を選択(USBシリアル出力を有効にするため)
*	  CPU Frequency:"80MHz(WiFi)"を選択(発熱を少しでも減少させるため)
*	  Partition Scheme:"MinimalSPIFFS(1.9MB APP with OTA/190kB SPIFFS))"を選択
*	  (プログラムサイズが大きいのでDefaultでは入らないため)
*
*	・外部温度計LYWSD03MMCはカスタムファームpvvxでフラッシュする
*		https://github.com/pvvx/ATC_MiThermometer
*	  Bluetooth Advertising FormatsはCustom format(pvvx)に設定
*	  Comfort parametersをTemperature Lo:18 Hi:28 Humidity Lo:25 Hi:75に変更
*
*	rev1.0	2024/01/13	initial revision by	Toshi
*	rev1.1	2024/01/23	ESP32C3対応
*	rev1.2	2024/02/02	外部温度計BLE接続対応
*	rev1.3	2024/02/04	ESP-NOWをESP8266のサブ基板経由で受け取る
*
*****************************************************************************/
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <EEPROM.h>
#include <ESP_Mail_Client.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <ESPping.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <driver/temp_sensor.h>

/********* 定数とマクロ *********/
#define	TRUE	true
#define	FALSE	false
#define	ERR		(-1)
#define	OK		0
#define	YES		1
#define	NO		0
#define BOOL	bool
#define CHAR	char
#define UCHAR	u_char
#define SHORT	int16_t
#define USHORT	uint16_t
#define LONG	int32_t
#define ULONG	uint32_t
#define FLOAT	float
#define DOUBLE	double
#define BOOLSET(x) (BOOL)(((x) != 0) ? TRUE : FALSE)
#define Print(...)   Serial.print(__VA_ARGS__), Client.print(__VA_ARGS__)
#define Println(...) Serial.println(__VA_ARGS__), Client.println(__VA_ARGS__)
#define Printf(...)  Serial.printf(__VA_ARGS__), Client.printf(__VA_ARGS__)
#define Write(...)  Serial.write(__VA_ARGS__), Client.write(__VA_ARGS__)

#define SMTP_HOST "smtp.gmail.com"				// SMTPサーバーのURL
#define SMTP_PORT esp_mail_smtp_port_465		// SMTPサーバーのポート番号
#define AUTHOR_EMAIL "xxxxxxxxxx@gmail.com"		// 送信元のメールアドレス
#define AUTHOR_PASSWORD "xxxxxxxxxxxxxxxx"		// 送信元のメールパスワード

#define CLOCK_INTERVAL (12L * 3600L)			// 最短時計調整インターバル[s]
#define PING_INTERVAL 10						// pingインターバル[分]
#define PHONE_FAILTIME (3L * 24L * 3600L)		// スマホ故障判定時間[s]
#define PHONE_YES (3L * PING_INTERVAL * 60L)	// スマホ通信あり判定時間[s]

// EEPROMデフォルト値
#define DEFAULT_MAIL_INTERVAL 4		// メール送信インターバル[h]
#define DEFAULT_SLEEP_HOUR 22		// タイマー停止時刻[h]
#define DEFAULT_WAKE_HOUR 6			// タイマー開始時刻[h]
#define DEFAULT_HEATUP_TEMP 31		// 熱中症警報温度[℃]
#define DEFAULT_HEATUP_TIME 30		// 熱中症警報時間[分]
// 送信先のメールアドレス0
#define DEFAULT_RECIPIENT_EMAIL0 "xxxxxx@gmail.com"
// 送信先のメールアドレス1
#define DEFAULT_RECIPIENT_EMAIL1 ""
// 送信先のメールアドレス2
#define DEFAULT_RECIPIENT_EMAIL2 ""
#define DEFAULT_T_OFFSET 0			// 内部温度センサーのオフセット[℃]
#define DEFAULT_BRIGHT_ON_TIME 12	// 連続点灯警告時間[h]
#define DEFAULT_BRIGHT_OFF_TIME 12	// 連続消灯警告時間[h]
#define DEFAULT_BRIGHT_LEVEL 1200	// CdS明るさ検知レベルA/D値(ESP32)
// 温度計Xiaomi LYWSD03MMC(カスタムファームpvvxでフラッシュ)
#define DEFAULT_DEVICENAME "ATC_AABBCC"	// 外部温度計デバイス名
#define DEFAULT_HOSTNAME "mimamori32"	// WiFiホスト名

// メール要因
#define MSTAT_NOSENSE	0x0001		// モーションセンサ長時間未検知
#define MSTAT_NOWAKE	0x0002		// モーションセンサ朝未検知
#define MSTAT_RESENSE	0x0004		// モーションセンサ再検知
#define MSTAT_HEATUP	0x0008		// 熱中症注意
#define MSTAT_FIRE		0x0010		// 火災感知
#define MSTAT_BRIGHTON	0x0020		// 連続点灯注意
#define MSTAT_BRIGHTOFF	0x0040		// 連続消灯注意

/********* ESP ピン設定 *********/
#define SENSE 1		// モーションセンサー
#define CDS 3		// CdS(+3.3Vとの間にCdSを接続し1MΩでプルダウン)
#define LED 10		// 外付けLED
#define LED_ON  digitalWrite(LED, 0);
#define LED_OFF digitalWrite(LED, 1);

/********* グローバル変数 *********/
SMTPSession smtp;	// SMTPセッションのオブジェクト
const String Sname0 = "見守りセンサー";		// システムの名前(実家)
const String Sname1 = "見守りセンサー開発中";// システムの名前(自宅)
const String Ssid0 = "SSID_JIKKA";		// WiFi SSID(実家)
const String Ssid1 = "SSID_JITAKU";		// WiFi SSID(自宅)
const String Pass0 = "pass_jikka";		// WiFi key(実家)
const String Pass1 = "pass_jitaku";		// WiFi key(自宅)
// スマホ側を固定IPの設定にしておく
const String MobileIp0 = "192.168.11.128";	// スマホIP(実家)
const String MobileIp1 = "192.168.1.127";	// スマホIP(自宅)

// デプロイ ウェブアプリURL(実家用)
const String DeployUrl0 = "https://script.google.com/macros/s/xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx/exec";

// デプロイ ウェブアプリURL(自宅テスト用)
const String DeployUrl1 = "https://script.google.com/macros/s/xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx/exec";

#define MAX_SHEET 6		// GoogleSpreadSheet最大シート数
// データシート名
const CHAR* SheetName[MAX_SHEET] = {"リビング", "廊下", "勝手口", "スマホ",
									"温度", "明度"};
BOOL fLocHome;					// 自宅ならTRUE
BOOL fInited;					// 通電後初回でない
volatile time_t LastSenseTime;	// 最後に検知した時間
volatile UCHAR LastSensor;		// 最後に検知したセンサー番号0,1,2
volatile BOOL fSense0;			// 親機が検知した
BOOL fSense12;					// 子機が検知した
BOOL fNoSense;			// モーションセンサー未検知状態
FLOAT Temperature;		// 温度[℃]
FLOAT TemperatureRaw;	// 温度(内部)生[℃]
FLOAT TemperatureInt;	// 温度(内部)[℃]
FLOAT TemperatureExt;	// 温度(外部)[℃]
FLOAT HumidityExt;		// 湿度(外部)[%]
UCHAR BattExt;			// 温度計バッテリー残量[%]
BOOL fDispTemperature;	// 温度表示モード
BOOL fWriteReq;			// SpreadSheet強制書き込みリクエスト(デバッグ用)
BOOL fStayHome = TRUE;	// 在宅判定(スマホが家にある)
ULONG NoPhoneCount;		// スマホが家にない時間[s]
const CHAR *SensorName[3] = {
 "リビングのセンサー", "廊下のセンサー", "勝手口のセンサー"};	// センサー名
volatile BOOL fBright;	// 周囲が明るい
volatile BOOL fInBedTime;// 睡眠時間帯
BOOL fHeatRange;		// 熱中症範囲フラグ
BOOL fHeatUp;			// 熱中症警報フラグ
BOOL fFire;				// 火災感知フラグ
SHORT AdCdS;			// CdS A/D読み取り値
ULONG BrightOnCount;	// 照明が点灯している時間[s]
ULONG BrightOffCount;	// 照明が消灯している時間[s]
BOOL fDispBright;		// 明るさ表示モード
size_t HeapSize1st;		// 初期ヒープサイズ
BOOL fRxExtTemp;		// 外部温度計から受信
SHORT NoRxTempCount;	// 外部温度計未受信カウンタ
BOOL fReqInit;			// 強制初期化要求
SHORT NoWakeCount;		// 朝になっても起きてこない時間[s]
BOOL fReSenseMail;		// モーションセンサー再検知

// WiFiサーバー
#define SERVER_PORT 54321
WiFiServer server(SERVER_PORT);
WiFiClient Client;

// ワークRAMサイズ
#define WORKSIZE 256

// OTA
BOOL fReqOTA;			// OTAによるプログラム書き換え要求
CHAR ProgramId[64];		// GoogleDriveファイルID

// BLE
BLEScan* pBLEScan;

// 設定パラメータやホットスタート前にEEPROMにバックアップされる変数
struct EEPstruct
{
	// 設定値(GoogleSpreadSheetからも設定可能)
	SHORT MailInterval;			// モーションセンサチェック間隔[h]
	SHORT SleepHour;			// 就寝時刻[h]
	SHORT WakeHour;				// 起床時刻[h]
	CHAR Mail0[32];				// 送信先メールアドレス0
	CHAR Mail1[32];				// 送信先メールアドレス1
	CHAR Mail2[32];				// 送信先メールアドレス2
	BOOL fUseCdS;				// CdS使う
	BOOL fUsePhone;				// スマホ検知使う
	SHORT HeatUpTemperature;	// 熱中症判定温度[℃]
	SHORT HeatUpTime;			// 熱中症判定時間[分]
	SHORT BrightOnTime;			// 連続点灯警告時間[h]
	SHORT BrightOffTime;		// 連続消灯警告時間[h]
	FLOAT Toffset;				// 内部温度センサーのオフセット[℃]
	SHORT BrightLevel;			// CdS明るさ判定レベルA/D値(ESP32のみ)
	// 設定値(シリアルのみで設定可能)
	CHAR DeviceName[16];		// 外部温度計名
	CHAR HostName[32];			// WiFiホスト名
	// バックアップされる変数
	time_t LastSetTime;			// 最後に時刻調整した時間
	ULONG NoSenseTimer;			// センサー未検知時間[s]
	SHORT InLivingTime;			// リビング滞在時間
	USHORT StatMail;			// メール要因
	UCHAR RetryCount;			// リトライカウント
	BOOL fHotStart;				// ホットスタート
	BOOL fWebAccess;			// GoogleSpreadSheetアクセス中
	UCHAR CountStat[3][24];		// 検知回数の統計データ
	UCHAR CountPhone[24];		// スマホ検知回数の統計データ
	SHORT Temp100[24];			// 温度の100倍値の時間ごとデータ
	UCHAR CountBright[24];		// 明るさ検知回数の統計データ
	SHORT YearZ;				// 昨日の年
	SHORT MonthZ;				// 昨日の月
	SHORT DayZ;					// 昨日の日
	ULONG Sum;
} Val;

/********* プロトタイプ宣言 *********/
IRAM_ATTR void SenseInterrupt();
void SetClock(EEPstruct* v);
void WiFiConnect();
void WiFiWait();
void CheckWiFi();
void smtpCallback(SMTP_Status status);
CHAR* StrDate(time_t tim);
void EepWrite();
void EepInitialWrite();
void EepRead();
void RxCmd();
void RxSensor(BOOL* fsense12, volatile UCHAR* lastsensor);
void smtpCallback(SMTP_Status status);
void SendMail(USHORT mstat);
void WriteSheet(SHORT y, SHORT m, SHORT d, SHORT st, EEPstruct* v);
void ReadSheet(BOOL* finit, CHAR* progid, BOOL* fota, EEPstruct* v);
void SheetAccess(SHORT y, SHORT m, SHORT d, BOOL fc, EEPstruct* v);
CHAR* strtokComma(CHAR* s);
void PrintStat(EEPstruct* v);
void ParseCommand(CHAR* rxbuff, EEPstruct* v);
SHORT CalcLivingTime(BOOL fsense);
void Mydelay(ULONG tim);
void PrintHeap();
void HotStart();
String urlEncode(const char *msg);
void MyStrcat(CHAR* d, CHAR* s);
BOOL PingPhone();
void AddOnTime(BOOL flag, SHORT* ontime);
FLOAT Average(FLOAT dat, BOOL finit);
void Wait1sec(ULONG t0, BOOL f1, BOOL f2);
void RemoteOTA();
void CheckInit();
void DispStatus();
void ReqSendMail(EEPstruct* v, BOOL fstayhome, ULONG lastsensetime,
					SHORT nowakecount, BOOL ffire, BOOL fheatup,
					SHORT brightoncount, SHORT brightoffcount,
					BOOL* fnosense, BOOL* fresensemail);
void SetAlarmFlags(EEPstruct* v, FLOAT t, FLOAT tint, FLOAT text,
					BOOL* fheatrange, BOOL* fheatup, BOOL* ffire);
FLOAT GetTemperature(EEPstruct* v, BOOL frxexttemp, FLOAT text,
					FLOAT* traw, FLOAT* tint);
size_t GetHeapSize();
void OTAupdate(CHAR* id);
static void scanCompleteCB(BLEScanResults scanResults);
void InitBLE();
void initTempSensor();
FLOAT mapf(FLOAT x, FLOAT in_min, FLOAT in_max, FLOAT out_min, FLOAT out_max);

/*----------------------------------------------------------------------------
	セットアップ
----------------------------------------------------------------------------*/
void setup()
{
	pinMode(SENSE, INPUT);
	pinMode(CDS, ANALOG);
	pinMode(LED, OUTPUT);
	LED_OFF

	// 内部温度センサーのイニシャライズ
	initTempSensor();

	//delay(2000);	// ESP8266はブート時に74880bpsでデータが来るので

	// ハードウェアシリアル
	Serial.begin(115200);	// 動作確認＆設定用
	Serial0.begin(115200);	// ESP-NOW受信用
	Println(F("*** reset ***"));

	// EEPROMからデータを読み出す
    EEPROM.begin(1024); // ESPはサイズ初期化が必要
#if 0
	EepInitialWrite();	// 構造体の中身を変更した後は一度有効にしてフラッシュ
#endif
	EepRead();
	fInited = TRUE;		// 初期データ読み込み完了

	// WiFi開始
	WiFiConnect();

	// WiFi切断時に自動復帰 WiFi.setAutoReconnect(TRUE);と同じ？
	MailClient.networkReconnect(TRUE);

	// WiFiサーバー開始
	server.begin();

	// 時計を合わせる
	SetClock(&Val);

	// WiFi OTA設定
	// (MDNS.beginがArduinoOTAの内部で行われる)
	ArduinoOTA.setHostname(Val.HostName);
	ArduinoOTA.setPassword(fLocHome ? Pass1.c_str() : Pass0.c_str());
	ArduinoOTA.begin();

	// WiFi OTAの状況の表示
	ArduinoOTA.onStart([]() {
		Println(F("OTA Start."));
		Val.fHotStart = TRUE;	// 次回起動時にホットスタートとする
		EepWrite();				// EEPROM書き換え
	});
	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		static SHORT pctz;
		SHORT pct = (SHORT)(progress / (total / 100));
		if (pct != pctz)
		{
			Print(F("OTA Progress: "));
			Print(pct);
			Println(F("%"));
			pctz = pct;
		}
	});
	ArduinoOTA.onEnd([]() {
		Println(F("OTA End."));
	});

	Print(F("WiFiホスト名="));
	Print(Val.HostName);
	Print(F(".local, ポート="));
	Println(SERVER_PORT);

	// メール送信中のホットスタートでリトライ数が3以下なら
	if (Val.StatMail && Val.RetryCount <= 3)
	{
		Println(F("メールを再送信します。"));
		SendMail(Val.StatMail);	// メール送信
		Val.StatMail = 0;
	}
	// GoogleSpreadSheetアクセス中のホットスタートでリトライ数が3以下なら
	if (Val.fWebAccess && Val.RetryCount <= 3)
	{
		Println(F("GoogleSpreadSheetへの操作を継続します。"));
		// GoogleSpreadSheet書き込みと読み込み
		SheetAccess(Val.YearZ, Val.MonthZ, Val.DayZ, TRUE, &Val);
		Val.fWebAccess = FALSE;
	}
	Val.RetryCount = 0;	// リトライカウンタクリア

	// BLEでXiaomiの温度計LYWSD03MMCと通信設定
	InitBLE();

	// モーションセンサー割り込み開始
	attachInterrupt(SENSE, SenseInterrupt, CHANGE);

	HeapSize1st = GetHeapSize();	// 初期ヒープサイズ
	// ヒープ表示
	PrintHeap();
}
/*----------------------------------------------------------------------------
	メインループ(1sごとに実行)
----------------------------------------------------------------------------*/
#define ACK 0			// 子機に返すACKキャラクタ
void loop()
{
	struct tm *tm;
	SHORT year, month, day, hour, minute;
	static SHORT hourz, minutez;
	static BOOL fsensled;
	static BOOL fping;
	FLOAT tempave;
	static SHORT runtime;
	static SHORT count;
	static SHORT wifiofftime;
	static BOOL fwakeup = TRUE;

	ULONG t0 = millis();	// ループ開始時刻

	time_t nowt = time(NULL);
	tm = localtime(&nowt);		// 現在時刻
	year = tm->tm_year + 1900;	// 年
	month = tm->tm_mon + 1;		// 月
	day = tm->tm_mday;			// 日
	hour = tm->tm_hour;			// 時
	minute = tm->tm_min;		// 分
	AddOnTime(TRUE, &runtime);	// オンしてからの時間[s]

	// 睡眠時間帯か？(就寝時刻以降か起床時間未満)
	fInBedTime = hour >= Val.SleepHour || hour < Val.WakeHour;

	/************************************************************************/
	// 時刻取得済みで日付が変わったかデバッグ用強制書き込み要求？
	if ((Val.YearZ > 2000 && day != Val.DayZ) || fWriteReq)
	{
		Val.fWebAccess = TRUE;	// Webアクセス中フラグセット
		// GoogleSpreadSheet書き込みと読み込み
		SheetAccess(Val.YearZ, Val.MonthZ, Val.DayZ, !fWriteReq, &Val);
		fWriteReq = FALSE;		// デバッグ用書き込み要求フラグクリア
		Val.fWebAccess = FALSE;	// Webアクセス中フラグクリア
		fwakeup = FALSE;		// この時間には就寝したものとする
	}
	/************************************************************************/
	// 親機または子機のモーションセンサが検知した？
	if (fSense0 || fSense12)
	{
		if (nowt > (24L * 3600L))	// 時計がセット済み？
		{
			LastSenseTime = nowt;	// 検知した日時を記憶
			Val.NoSenseTimer = 0L;		// モーションセンサー未検知時間クリア
			if (fNoSense)			// 未検知中の検知なら
			{
				fReSenseMail = TRUE;// センサーが再検知したメール送信要求
				fNoSense = FALSE;	// 未検知状態クリア
			}
			// 朝(起床時刻-2時間)以降にモーションセンサが検知した？
			if (hour >= Val.WakeHour - 2)
			{
				fwakeup = TRUE;	// 起床した
			}

			// 検知回数の統計
			if (Val.CountStat[LastSensor][hour] < 255)
			{
				Val.CountStat[LastSensor][hour]++;
			}
			if (LastSensor)		// 検知したのは子機？
			{
				fsensled = TRUE;// LED点滅用
			}
			// モーションを検知した時刻を表示
			Print(SensorName[LastSensor]);
			Print(F("検知:"));
			Println(StrDate(LastSenseTime));

			// ヒープ表示
			PrintHeap();
		}
	}
	// 起床時間を過ぎても起床していない時間[s]
	AddOnTime(!fwakeup && hour >= Val.WakeHour, &NoWakeCount);

	// リビング滞在時間を算出
	Val.InLivingTime = CalcLivingTime(fSense0);

	/************************************************************************/
	// スマホが家にあるかどうかのチェックのためpingを送る
	// 時間が来たなら
	if (Val.YearZ > 2000 && minute != minutez && (minute % PING_INTERVAL) == 0)
	{
		fping = PingPhone();
		if (fping)
		{
			Val.CountPhone[hour]++;
		}
	}
	// スマホを使う設定ならばスマホが検知できない時間を累積
	if (Val.fUsePhone)
	{
		// 家にない時間の累積
		if (!fping)
		{
			// 最大値に達していないなら
			if (NoPhoneCount < PHONE_FAILTIME)
			{
				NoPhoneCount++;	// 累積
			}
		}
		else
		{
			NoPhoneCount = 0L;	// スマホが家にあるなら0
		}
	}
	else
	{
		NoPhoneCount = 0L;	// スマホを使わない設定なら0
	}
	// 在宅判定(スマホが宅内にあるかの判定)
	// スマホがスリープしてpingの応答がなくても少しの期間は継続
	// 長期間応答がない時は電源オフか電池切れとみなして宅内にあるものとする
	fStayHome = NoPhoneCount <= PHONE_YES || NoPhoneCount >= PHONE_FAILTIME;

	/************************************************************************/
	// 温度[℃]
	Temperature = GetTemperature(&Val, fRxExtTemp, TemperatureExt,
								&TemperatureRaw, &TemperatureInt);
					
	if (runtime >= 20)	// 最初のうちは外部温度が得られない
	{
		// 時間ごと平均値
		tempave = Average(Temperature, BOOLSET(hourz != hour));
		// 時間ごとの温度の平均値を記録
		Val.Temp100[hour] = (USHORT)(tempave * 100.0);	// 温度[℃]x100
	}
	// 温度計のアドバタイズフレームをスキャン
	if (++count >= 10)	// 10秒に1回
	{
		count = 0;

		// 温度計のアドバタイズ間隔が2.5[s]なので最大3秒スキャン
		// ↓スキャン終了時のコールバックを指定すると内部で待たずにすぐに戻る
		// (この間ループは回るがESP-NOWの受信はできない→ESP-NOWは別ボードに)
		pBLEScan->start(3, scanCompleteCB, false);
	}

	/************************************************************************/
	// 明るさ検知と連続点灯＆連続消灯時間をカウント
	AdCdS = analogRead(CDS);
	fBright = AdCdS >= Val.BrightLevel;	// 周囲が明るければTRUE
	if (fStayHome)	// 在宅の場合
	{
		if (fBright)	// 明るい→昼間または夜間に照明がオンしているなら
		{
			if (fInBedTime)	// 寝ている時間帯か？
			{
				BrightOnCount++;	// 点灯注意カウント++
			}
			BrightOffCount = 0L;	// 消灯注意カウントはクリア
		}
		else	// 暗い→カーテンが開いていないか夜間照明がオフなら
		{
			BrightOnCount = 0L;	// 点灯注意カウントはクリア
			if (!fInBedTime)	// 起きている時間帯か？
			{
				BrightOffCount++;	// 消灯注意カウント++
			}
		}
	}
	else	// 外出したら点灯＆消灯注意カウントリセット
	{
		BrightOnCount = BrightOffCount = 0L;
	}
	// 1分ごとに明るさ検知回数の統計
	if (Val.YearZ > 2000 && minute != minutez)
	{
		if (fBright)
		{
			Val.CountBright[hour]++;
		}
	}

	/************************************************************************/
	// 起床中かつ在宅か
	if (!fInBedTime && fStayHome)
	{
		Val.NoSenseTimer++;	// モーション無検知カウンターインクリメント
	}

	/************************************************************************/
	// 各種警報フラグを立てる
	SetAlarmFlags(&Val, Temperature, TemperatureInt, TemperatureExt,
				&fHeatRange, &fHeatUp, &fFire);

	/************************************************************************/
	// 条件が揃ったらメール送信要求
	ReqSendMail(&Val, fStayHome, LastSenseTime, NoWakeCount, fFire, fHeatUp,
				BrightOnCount, BrightOffCount,
				&fNoSense, &fReSenseMail);

	/************************************************************************/
	// その他の処理
	//
	// フェールセーフ：WiFiオフが長時間なら再接続
	AddOnTime(WiFi.status() != WL_CONNECTED, &wifiofftime);	// WiFiオフ時間[s]
	if (wifiofftime >= 300)	// 5分以上自動で再接続しなかった？
	{
		wifiofftime = 0;
		WiFiConnect();		// 再度接続を試みる
	}
	// フェールセーフ：時刻が23:55になった時にヒープが半減していたら再起動
	if (hour == 23 && minutez == 54 && minute == 55 &&
		GetHeapSize() < HeapSize1st / 2)
	{
		HotStart();	// ホットスタート
	}

	// 時刻が01:01になった時に前回から十分時間が経過していれば時計を合わせる
	// (時刻が戻されても2回呼ばないように)
	if (hour == 1 && minutez == 0 && minute == 1 &&
		nowt > Val.LastSetTime + CLOCK_INTERVAL)
	{
		SetClock(&Val);		// 時計を合わせる
	}

	// 温度計から未受信が連続した？
	if (NoRxTempCount > 60)
	{
		fRxExtTemp = FALSE;		// 外部温度計受信フラグクリア
	}
	else
	{
		NoRxTempCount++;
	}

	// ステータスの変化などを表示
	DispStatus();

	// OTAリクエスト(GoogleSpreadSheetで設定)があった時の処理
	// GoogleSpreadSheetからのOTA要求
	if (fReqOTA)
	{
		fReqOTA = FALSE;
		RemoteOTA();	// OTA処理
	}

	// 初期化リクエスト(GoogleSpreadSheetで設定)があった時の処理
	CheckInit();

	// 今回値をメモリ
	Val.YearZ = year;
	Val.MonthZ = month;
	Val.DayZ = day;
	hourz = hour;
	minutez = minute;

	// 今回の検知状態をクリア
	fSense0 = fSense12 = FALSE;

	//Print(F("処理時間[ms]="));
	//Println(millis() - t0);

	// 1秒待つ
	Wait1sec(t0, WiFi.status() != WL_CONNECTED, fsensled);
	fsensled = FALSE;
}
/*----------------------------------------------------------------------------
	内部温度センサーのイニシャライズ
	外気温より60℃ほど高くなるので、DACは高温を読み取る設定とする
	書式 void initTempSensor();
----------------------------------------------------------------------------*/
void initTempSensor()
{
	temp_sensor_config_t temp_sensor = TSENS_CONFIG_DEFAULT();
	temp_sensor.dac_offset = TSENS_DAC_L0;	// (50～125℃)
	temp_sensor_set_config(temp_sensor);
	temp_sensor_start();
}
/*----------------------------------------------------------------------------
	温度を取得する
	外部センサーから受信していたらそちらを優先
	書式 ret = FLOAT GetTemperature(EEPstruct* v, BOOL frxexttemp, FLOAT text,
						FLOAT* traw,  FLOAT* tint);

	FLOAT ret;			温度[℃]
	EEPstruct* vl;		Toffset 内部温度センサーオフセット調整値
	BOOL frxexttemp;	外部温度計受信フラグ
	FLOAT text;			外部温度センサーの温度
	FLOAT* traw;		内部温度の生値
	FLOAT* tint;		内部温度センサーの温度
----------------------------------------------------------------------------*/
#define INMIN 60.0
#define INMAX 90.0
#define OUTMIN 55
#define OUTMAX 61
FLOAT GetTemperature(EEPstruct* v, BOOL frxexttemp, FLOAT text,
						FLOAT* traw,  FLOAT* tint)
{
	static FLOAT tempr = 20.0;
	FLOAT x, ofst;

	temp_sensor_read_celsius(&x);	// チップ内温度
	*traw = x;						// 生データ
	ofst = mapf(x, INMIN, INMAX, OUTMIN, OUTMAX);	// オフセット分
	x -= ofst;	// オフセットを差し引く
	#if 1
	tempr = 0.3 * x + 0.7 * tempr;	// フィルタリング
	#else
	tempr = x;						// フィルタリングなし
	#endif
	*tint = tempr - v->Toffset;	// 内部センサーによる外気温[℃]
	return frxexttemp ? text : *tint;	// 外部センサー受信ならそちらを優先
}
/*----------------------------------------------------------------------------
	各種警報フラグを立てる
	書式 void SetAlarmFlags(EEPstruct* v, FLOAT t, FLOAT tint, FLOAT text,
							BOOL* fheatrange, BOOL* fheatup, BOOL* ffire);

	EEPstruct* v;		設定値
	FLOAT t;			温度
	FLOAT tint;			内部温度センサーの温度
	FLOAT test;			外部温度センサーの温度
	BOOL* fheatrange;	熱中症温度範囲
	BOOL* fheatup;		熱中症警報
	BOOL* ffire;		火災感知
----------------------------------------------------------------------------*/
#define FIRE_TEMP 50	// 火災感知温度[℃]
void SetAlarmFlags(EEPstruct* v, FLOAT t, FLOAT tint, FLOAT text,
					BOOL* fheatrange, BOOL* fheatup, BOOL* ffire)
{
	// 熱中症危険範囲
	if (!*fheatrange && t > v->HeatUpTemperature)
	{
		*fheatrange = TRUE;	// 熱中症危険範囲フラグオン
	}
	else if (*fheatrange && t <= (v->HeatUpTemperature - 3.0))
	{
		*fheatrange = FALSE;	// 熱中症危険範囲フラグオフ
	}
	// 熱中症危険範囲でリビングに長時間人がいる
	if (!*fheatup && *fheatrange && v->InLivingTime >= (v->HeatUpTime * 60))
	{
		*fheatup = TRUE;	// 熱中症警戒フラグオン
	}
	else if (*fheatup && !*fheatrange)	// 熱中症危険範囲を脱した
	{
		*fheatup = FALSE;// 熱中症警戒フラグオフ
	}
	// 火災感知(内部センサーと外部センサーのハイセレ)
	FLOAT tempmax = max(tint, text);
	if (!*ffire && tempmax >= FIRE_TEMP)
	{
		*ffire = TRUE;	// 火災感知フラグオン
	}
	else if (*ffire && tempmax <= (FIRE_TEMP - 10))
	{
		*ffire = FALSE;	// 火災感知フラグオフ
	}
}
/*----------------------------------------------------------------------------
	条件が揃ったらメール送信要求
	書式 void ReqSendMail(EEPstruct* v, BOOL fstayhome, ULONG lastsensetime,
						SHORT nowakecount, BOOL ffire, BOOL fheatup,
						SHORT brightoncount,SHORT brightoffcount,
						BOOL* fnosense, BOOL* fresensemail);

	EEPstruct* v;			設定値
	BOOL fstayhome;			在宅判定
	ULONG lastsensetime;	前回検知時刻
	SHORT nowakecount;		朝になっても起きてこない時間[s]
	BOOL ffire;				火災感知
	BOOL fheatup;			熱中症警報
	SHORT brightoncount;	連続点灯時間[s]
	SHORT brightoffcount;	連続消灯時間[s]
	BOOL* fnosense;			センサー未検知中
	BOOL* fresensemail;		再検知フラグ
----------------------------------------------------------------------------*/
void ReqSendMail(EEPstruct* v, BOOL fstayhome, ULONG lastsensetime,
				SHORT nowakecount, BOOL ffire, BOOL fheatup,
				SHORT brightoncount, SHORT brightoffcount,
				BOOL* fnosense, BOOL* fresensemail)
{
	static ULONG boncountz, boffcountz;
	static BOOL fheatupz, ffirez;

	// 在宅でセンサー検知が規定時間以上なかったら
	if (fstayhome && lastsensetime > (24L * 3600L) && v->NoSenseTimer > 0L &&
		v->NoSenseTimer % ((LONG)v->MailInterval * 3600L) == 0)
	{
		Println(F("検知せずのメールを送信します。"));
		v->StatMail |= MSTAT_NOSENSE;	// モーション未検知メール送信要求
		SendMail(v->StatMail);			// メール送信
		v->StatMail &= ~MSTAT_NOSENSE;
		*fnosense = TRUE;		// センサー未検知中
	}
	// 在宅で起床時間を過ぎても起床していない時間が1時間たったら
	if (fstayhome && nowakecount > 0 && nowakecount % 3600 == 0)
	{
		Println(F("朝起きてこないメールを送信します。"));
		v->StatMail |= MSTAT_NOWAKE;	// 起きてこないメールを送信要求
		SendMail(v->StatMail);			// メール送信
		v->StatMail &= ~MSTAT_NOWAKE;
		*fnosense = TRUE;		// センサー未検知中
	}
	// センサー未検知中に再検知したらメール送信
	if (*fresensemail)
	{
		Println(F("再検知のメールを送信します。"));
		v->StatMail |= MSTAT_RESENSE;	// 再検知したらメールを送信信要求
		SendMail(v->StatMail);			// メール送信
		v->StatMail &= ~MSTAT_RESENSE;
		*fresensemail = FALSE;	// 再検知フラグクリア
	}
	// 火災を感知した瞬間
	if (ffire & !ffirez)
	{
		Println(F("火災感知のメールを送信します。"));
		v->StatMail |= MSTAT_FIRE;		// 火災感知メール送信要求
		SendMail(v->StatMail);			// メール送信
		v->StatMail &= ~MSTAT_FIRE;
	}
	// 熱中症警報が出た瞬間
	if (fheatup & !fheatupz)
	{
		Println(F("熱中症注意のメールを送信します。"));
		v->StatMail |= MSTAT_HEATUP;	// 熱中症注意メール送信要求
		SendMail(v->StatMail);			// メール送信
		v->StatMail &= ~MSTAT_HEATUP;
	}
	// 連続点灯注意時間に達した瞬間
	if (boncountz < v->BrightOnTime * 3600 &&
		brightoncount >= v->BrightOnTime * 3600)
	{
		Println(F("連続点灯注意のメールを送信します。"));
		v->StatMail |= MSTAT_BRIGHTON;	// 連続点灯注意のメール送信要求
		SendMail(v->StatMail);			// メール送信
		v->StatMail &= ~MSTAT_BRIGHTON;
	}
	// 連続消灯注意時間に達した瞬間
	if (boffcountz < v->BrightOffTime * 3600 &&
		brightoffcount >= v->BrightOffTime * 3600)
	{
		Println(F("連続消灯注意のメールを送信します。"));
		v->StatMail |= MSTAT_BRIGHTOFF;	// 連続消灯注意のメール送信要求
		SendMail(v->StatMail);				// メール送信
		v->StatMail &= ~MSTAT_BRIGHTOFF;
	}
	boncountz = brightoncount;
	boffcountz = brightoffcount;
	fheatupz = fheatup;
	ffirez = ffire;
}
/*----------------------------------------------------------------------------
	ステータスの変化などを表示
	書式 void DispStatus();

	グローバル変数:read
	BOOL fDispBright;		明るさを表示する
	BOOL fBright;			明るいと判定されている
	SHORT AdCdS;			CdS A/Dの生値
	BOOL fDispTemperature;	温度を表示する
	FLOAT TemperatureExt;	外部温度センサーの温度
	FLOAT TemperatureInt;	内部温度センサーの温度
----------------------------------------------------------------------------*/
void DispStatus()
{
	USHORT stat = WiFi.status();
	static USHORT statz;

	if (stat != statz)		// WiFiステータスが変化した？
	{
		Print(F("WiFi.status = "));
		Println(stat);
	}
	if (fDispBright)		// 明るさを表示する？
	{
		Print(F("明度="));
		Print(fBright);
		Print(F(" A/D="));
		Println(AdCdS);
	}
	if (fDispTemperature)	// 温度を表示する？
	{
		Print(F("外部/内部温度="));
		Print(TemperatureExt);
		Print(F(" / "));
		Println(TemperatureInt);
	}
	statz = stat;
}
/*----------------------------------------------------------------------------
	GoogleSpreadSheetでOTAリクエストがあった時の処理
	書式 void RemoteOTA();
----------------------------------------------------------------------------*/
void RemoteOTA()
{
	if (strlen(ProgramId) != 0)
	{
		Println(F("OTAアップデートを開始します。"));
		OTAupdate(ProgramId);	// GoogleDriveのファイルでOTAアップデート

		// GoogleSpreadSheetのOTA要求フラグをクリアする
		WriteSheetCore("設定", "B17,0");	// fReqOTAのセル
		HotStart();	// ホットスタート
	}
}
/*----------------------------------------------------------------------------
	初期化要求があったらEEPを初期化してリセット
	書式 void CheckInit();
----------------------------------------------------------------------------*/
void CheckInit()
{
	// GoogleSpreadSheetからの初期化要求
	if (fReqInit)
	{
		fReqInit = FALSE;	// 念のため
		Println(F("初期化して再起動します。"));
		EepInitialWrite();	// EEPROM初期化
		// GoogleSpreadSheetの初期化要求フラグをクリアする
		WriteSheetCore("設定", "B7,0");	// 初期化要求のセル
		Println(F("5秒後に再起動します。"));
		delay(5000);
		ESP.restart();	// ESP再起動
	}
}
/*----------------------------------------------------------------------------
	1秒待つ
	書式 void Wait1sec(ULONG t0, BOOL f1, BOOL f2);

	ULONG t0;	メインループ開始時間[ms]
	BOOL f1;	WiFi非接続
	BOOL f2;	子機が検知した
----------------------------------------------------------------------------*/
void Wait1sec(ULONG t0, BOOL f1, BOOL f2)
{
	// 1000msからメイン処理に要した時間[ms]を差し引く
	ULONG t1 = 1000L - (millis() - t0);
	if (f1)	// WiFi非接続
	{
		LED_ON
		Mydelay(100);
		LED_OFF
		Mydelay(t1 - 100);
	}
	else if (f2)	// 子機が検知
	{
		LED_ON
		Mydelay(50);
		LED_OFF
		Mydelay(200);
		LED_ON
		Mydelay(50);
		LED_OFF
		Mydelay(t1 - 300);
	}
	else
	{
		Mydelay(t1);
	}
}
/*----------------------------------------------------------------------------
	スマホにpingを打つ
	書式 ret = PingPhone();

	BOOL ret;	pingに応答した

	グローバル変数:read
	BOOL fLocHome;		自宅(TRUE)か実家か(FALSE)
	String MobileIp0, MobileIp1;
----------------------------------------------------------------------------*/
BOOL PingPhone()
{
	IPAddress dest;

	// 文字列からIPAddressへ
	dest.fromString(fLocHome ? MobileIp1 : MobileIp0);
	Print(F("ping to "));
	Println(fLocHome ? MobileIp1 : MobileIp0);
	BOOL fping = Ping.ping(dest);	// pingを打つ
	Print(F("ping結果="));
	Println(fping);
	return fping;
}
/*----------------------------------------------------------------------------
	フリーヒープサイズを表示
	書式 void PrintHeap();
----------------------------------------------------------------------------*/
size_t GetHeapSize()
{
	return esp_get_free_heap_size();
}
void PrintHeap()
{
	Print(F("フリーヒープサイズ:"));
	Println(GetHeapSize());
}
/*----------------------------------------------------------------------------
	指定時間待つ(毎ループ実行する処理を行う)
	書式 void Mydelay(ULONG tim);

	ULONG tim;		待ち時間[ms]
----------------------------------------------------------------------------*/
void Mydelay(ULONG tim)
{
	ULONG t0 = millis();

	while (millis() < t0 + tim)
	{
		delay(1);
		RxCmd();				// コマンド受信処理
		RxSensor(&fSense12, &LastSensor);	// ESP-NOWシリアル受信処理
		ArduinoOTA.handle();	// WiFi OTA

		// WiFiが接続中なら
		if (WiFi.status() == WL_CONNECTED)
		{
			// WiFiクライアント取得
			if (!Client)
			{
				Client = server.accept();
			}
			// クライアントが切断された？
			if (Client && !Client.connected())
			{
				Println("クライアントが切断されました");
				// たまっているデータを削除
				// (2台接続した時に受信データを消去しないと次の読み取り不可)
				while (Client.available())
				{
					Client.read();
				}
			}
		}
	}
}
/*----------------------------------------------------------------------------
	リビングで過ごしている時間[s]を取得
	書式 ret = CalcLivingTime(BOOL fsense);

	SHORT ret;		経過時間[s]
	BOOL fsense;	リビングのセンサー検知フラグ
----------------------------------------------------------------------------*/
#define LIVETIMEUP (10 * 60)	// リビング退出判定時間[s]
SHORT CalcLivingTime(BOOL fsense)
{
	static BOOL ftim;
	static SHORT timer1, total;

	if (fsense)	// リビングのセンサーが検知した
	{
		if (!ftim)	// 今カウント中でないなら
		{
			ftim = TRUE;	// カウント開始
			total = 0;		// 累積時間をクリア
		}
		else	// 今カウント中だったなら
		{
			if (total < 30000) total += timer1;	// 累積時間をアップデート
			Print(F("リビング滞在時間[分]:"));
			Println(total / 60);
		}
		timer1 = 0;		// タイマークリア
	}
	if (ftim)	// カウント中なら
	{
		timer1++;	// タイマー++
	}
	if (timer1 >= LIVETIMEUP)	// タイムアップ？
	{
		ftim = FALSE;			// タイマー停止
		timer1 = total = 0;		// カウンターと累積時間をクリア
		Println(F("リビング退出"));
	}
	return total;	// 累積時間を返す
}
/*----------------------------------------------------------------------------
	統計データの表示
	書式 void PrintStat(EEPstruct* v);

	EEPstruct* v;				統計データ
----------------------------------------------------------------------------*/
void PrintStat(EEPstruct* v)
{
	SHORT i, j;
	CHAR str[10];
	CHAR workchar[WORKSIZE];	// 文字列ワークエリア

	for (i = 0; i < 3; i++)
	{
		strcpy(workchar, SensorName[i]);
		for (SHORT j = 0 ;j < 24; j++)
		{
			sprintf(str, ",%d", v->CountStat[i][j]);
			MyStrcat(workchar, str);
		}
		Println(workchar);
	}
	strcpy(workchar, "スマホ");
	for (j = 0 ;j < 24; j++)
	{
		sprintf(str, ",%d", v->CountPhone[j]);
		MyStrcat(workchar, str);
	}
	Println(workchar);

	strcpy(workchar, "温度");
	for (j = 0 ;j < 24; j++)
	{
		sprintf(str, ",%3.1f", (FLOAT)v->Temp100[j] / 100.0);
		MyStrcat(workchar, str);
	}
	Println(workchar);

	strcpy(workchar, "明度");
	for (j = 0 ;j < 24; j++)
	{
		sprintf(str, ",%d", v->CountBright[j]);
		MyStrcat(workchar, str);
	}
	Println(workchar);
}
/*----------------------------------------------------------------------------
	コマンド解析処理
	書式 void ParseCommand(CHAR* rxbuff, EEPstruct* v);

	CHAR* rxbuff;	受信バッファ
	EEPstruct* v;	設定データ
----------------------------------------------------------------------------*/
void ParseCommand(CHAR* rxbuff, EEPstruct* v)
{
	CHAR cmd, *p, num;
	SHORT i;

	if (rxbuff[0] == '-')	// コマンド行か？
	{
		cmd = rxbuff[1];	// コマンド文字
		p = &rxbuff[2];
		while (*p > 0 && *p <= ' ') p++;// 先頭の空白除去
		switch (cmd)
		{
			// デバッグ用ホットスタート
			case 'y':
				HotStart();	// 再起動
				break;
			// デバッグ用メール送信
			case 'z':
				if (*p == 'a')	// 全メール？
				{
					SendMail(MSTAT_NOSENSE);	// センサー未検知メール送信
					SendMail(MSTAT_NOWAKE);		// 朝起床せずメール送信
					SendMail(MSTAT_RESENSE);	// センサー再検知メール送信
					SendMail(MSTAT_HEATUP);		// 熱中症注意メール送信
					SendMail(MSTAT_FIRE);		// 火災感知メール送信
					SendMail(MSTAT_BRIGHTON);	// 照明連続オンメール送信
					SendMail(MSTAT_BRIGHTOFF);	// 照明連続オフメール送信
				}
				else
				{
					SendMail(MSTAT_NOSENSE);	// センサー未検知メール送信
				}
				break;
			// デバッグ用GoogleSpreadSheet書き込み
			case 'x':
				fWriteReq = TRUE;
				break;
			// デバッグ用GoogleSpreadSheet設定値読み込み
			case 'r':
				ReadSheet(&fReqInit, ProgramId, &fReqOTA, &Val);
				break;
			// スマホにpingを送る
			case 'p':
				PingPhone();
				break;
			// EEPデフォルトに
			case 'c':
				EepInitialWrite();
				break;
			// 不検知判定間隔[h]
			case 'i':
				i = atoi(p);
				if (i > 0)
				{
					v->MailInterval = i;
				}
				break;
			// 就寝時刻[h]
			case 's':
				i = atoi(p);
				if (i >= 0 && i <= 24)
				{
					v->SleepHour = i;
				}
				break;
			// 起床時刻[h]
			case 'w':
				i = atoi(p);
				if (i >= 0 && i <= 24)
				{
					v->WakeHour = i;
				}
				break;
			// CdSをリビングのセンサ検出時に使うか
			case 'd':
				i = atoi(p);
				if (i == 0 || i == 1)
				{
					v->fUseCdS = (BOOL)i;
				}
				break;
			// スマホの存在を使うか
			case 'f':
				i = atoi(p);
				if (i == 0 || i == 1)
				{
					v->fUsePhone = (BOOL)i;
				}
				break;
			// 温度センサーオフセット[℃]x10
			case 'o':
				if (strlen(p) == 0)		// パラメータなしなら
				{
					fDispTemperature ^= 1;	// 表示フラグトグル
				}
				else
				{
					FLOAT f = atof(p);
					v->Toffset = f;
				}
				break;
			// 熱中症警報温度[℃]
			case 'h':
				i = atoi(p);
				if (i >= 0 || i <= 40)
				{
					v->HeatUpTemperature = i;
				}
				break;
			// 熱中症警報時間[分]
			case 'k':
				i = atoi(p);
				if (i >= 0 || i <= 240)
				{
					v->HeatUpTime = i;
				}
				break;
			// 連続点灯注意時間[時]
			case 'g':
				i = atoi(p);
				if (i >= 0 || i <= 240)
				{
					v->BrightOnTime = i;
				}
				break;
			// 連続消灯注意時間[時]
			case 'j':
				i = atoi(p);
				if (i >= 0 || i <= 240)
				{
					v->BrightOffTime = i;
				}
				break;
			// 明るさ判定レベルA/D値
			case 'l':
				if (strlen(p) == 0)		// パラメータなしなら
				{
					fDispBright ^= 1;	// 表示フラグトグル
				}
				else
				{
					i = atoi(p);
					if (i >= 0 || i <= 4095)
					{
						v->BrightLevel = i;
					}
				}
				break;
			// 外部温度センサー名
			case 't':
				if (strlen(p) < 16)
				{
					v->DeviceName[0] = '\0';
					strcpy(v->DeviceName, p);
				}
				break;
			// WiFiホスト名
			case 'b':
				if (strlen(p) < 32)
				{
					v->HostName[0] = '\0';
					strcpy(v->HostName, p);
				}
				break;
			// mail URL
			case 'm':
				num = *p++;
				while (*p > 0 && *p <= ' ') p++;// 先頭の空白除去
				switch (num)
				{
					// Mail0書き換え
					case '0':
						if (strlen(p) < 32)
						{
							v->Mail0[0] = '\0';
							strcpy(v->Mail0, p);
						}
						break;
					// Mail1書き換え
					case '1':
						if (strlen(p) < 32)
						{
							v->Mail1[0] = '\0';
							strcpy(v->Mail1, p);
						}
						break;
					// Mail2書き換え
					case '2':
						if (strlen(p) < 32)
						{
							v->Mail2[0] = '\0';
							strcpy(v->Mail2, p);
						}
						break;
					default:
						break;
				}
			default:
				break;
		}
		// EEPROM内容の変更なら
		if (cmd == 'i' || cmd == 's' || cmd == 'w' ||
			cmd == 'd' || cmd == 'f' || cmd == 'o' ||
			cmd == 'h' || cmd == 'k' || cmd == 'g' ||
			cmd == 'j' || cmd == 'l' || cmd == 't' ||
			cmd == 'b' || cmd == 'm' || cmd == 'c')
		{
			EepRead();		// EEPROM書き込みと読み出し
		}
		if (cmd == 'b')		// WiFiホスト名の変更なら
		{
			HotStart();		// 再起動
		}
	}
	else if (rxbuff[0] == '?')
	{
		Println(SensorName[0]);
		Print(F("IP:"));
		Println(WiFi.localIP());
		Print(F("Channel:"));
		Println(WiFi.channel());
		Print(F("MAC(ST):"));
		Println(WiFi.macAddress());
		Print(F("全センサー無検知時間[分]:"));
		Println(v->NoSenseTimer / 60L);
		Print(F("在宅判定:"));
		Println(fStayHome);
		Print(F("明るさ検知:"));
		Println(fBright);
		Print(F("夜間明るさ検知時間[分]:"));
		Println(BrightOnCount / 60L);
		Print(F("昼間暗さ検知時間[分]:"));
		Println(BrightOffCount / 60L);
		Print(F("内部センサー温度生値[℃]:"));
		Println(TemperatureRaw);
		Print(F("内部センサー温度[℃]:"));
		Println(TemperatureInt);
		Print(F("外部センサー接続:"));
		Println(fRxExtTemp);
		Print(F("外部センサー温度[℃]:"));
		Println(TemperatureExt);
		Print(F("外部センサーバッテリー[%]:"));
		Println(BattExt);
		Print(F("熱中症温度範囲内:"));
		Println(fHeatRange);
		Print(F("リビング滞在時間[分]:"));
		Println(v->InLivingTime / 60);
		PrintHeap();		// ヒープ表示
		PrintStat(v);		// 統計データの表示
		Println(F("*** 設定コマンドと現在値 ***"));
		Print(F("-i 未検知チェック間隔[時間] ")); Println(v->MailInterval);
		Print(F("-s 就寝時刻[時] ")); Println(v->SleepHour);
		Print(F("-w 起床時刻[時] ")); Println(v->WakeHour);
		Print(F("-m0 メール送信先アドレス0 ")); Println(v->Mail0);
		Print(F("-m1 メール送信先アドレス1 ")); Println(v->Mail1);
		Print(F("-m2 メール送信先アドレス2 ")); Println(v->Mail2);
		Print(F("-d CdSを利用[0 or 1] ")); Println(v->fUseCdS);
		Print(F("-f スマホを利用[0 or 1] ")); Println(v->fUsePhone);
		Print(F("-o 内部温度オフセット(パラメータなしなら表示トグル) "));
		Println(v->Toffset);
		Print(F("-t 外部温度センサー名 ")); Println(v->DeviceName);
		Print(F("-b WiFiホスト名 ")); Println(v->HostName);
		Print(F("-h 熱中症警報温度[℃] ")); Println(v->HeatUpTemperature);
		Print(F("-k 熱中症警報時間[分間] ")); Println(v->HeatUpTime);
		Print(F("-g 連続点灯注意時間[時間] ")); Println(v->BrightOnTime);
		Print(F("-j 連続消灯注意時間[時間] ")); Println(v->BrightOffTime);
		Print(F("-l 明るさ判定A/D値(パラメータなしなら表示トグル) "));
		Println(v->BrightLevel);
		Println(F("*** デバッグ＆メンテ用 ***"));
		Println(F("-c EEPROMイニシャライズ"));
		Println(F("-p スマホにpingを打つ"));
		Println(F("-y ホットスタート"));
		Println(F("-z モーション検知せずのメール送信"));
		Println(F("-za 全メール送信"));
		Println(F("-x GoogleSpredSheet書き込み"));
		Println(F("-r GoogleSpredSheet設定読み込み"));
	}
}
/*----------------------------------------------------------------------------
	ホットスタート処理(再起動したときにも必要な変数の内容をキープ)
	書式 void HotStart();

	グローバル変数:read
	EEPstruct Val;	StatMail, fWebAccess メール送信中, シートアクセス中
	グローバル変数:write
	EEPstruct Val;	RetryCount メール送信中かシートアクセス中のリトライ回数
----------------------------------------------------------------------------*/
void HotStart()
{
	Println(F("\n5秒後に再起動します。"));
	Val.fHotStart = TRUE;	// 再起動であることを記録
	// メール送信中かシートアクセス中だったら
	if (Val.StatMail != 0 || Val.fWebAccess)
	{
		Val.RetryCount++;	// リトライカウンターを+1
	}
	EepWrite();			// EEP書き込み処理
	delay(5000);
	ESP.restart();		// ESP再起動
}
/*----------------------------------------------------------------------------
	USBシリアルおよびWiFiシリアルコマンド受信処理
	書式 void RxCmd();
----------------------------------------------------------------------------*/
#define RXMAX 80		// シリアル受信バッファサイズ
void RxCmd()
{
	CHAR c, *p;
	static CHAR rxbuff[RXMAX];		// シリアル受信バッファ
	static UCHAR rxptr;				// バッファポインタ

	// 文字を受信していれば
	while (Serial.available() > 0 ||
		  (Client && Client.connected() && Client.available()))
	{
		// 1文字受信
		if (Serial.available() > 0)
		{
			c = Serial.read();
		}
		else
		{
			c = Client.read();
		}
		Write(c);				// エコーバック
		if (rxptr < RXMAX - 2)	// バッファに余裕があるなら
		{
			rxbuff[rxptr++] = c;	// 格納
			if (c == '\n')			// 行末か？
			{
				rxbuff[rxptr] = '\0';	// 文字列をターミネート
				if ((p = strchr(rxbuff, '\r')) != NULL)
				{
					*p = '\0';		// CRを除去
				}
				ParseCommand(rxbuff, &Val);
				rxptr = 0;	// ポインタを先頭に
			}
		}
		else	// バッファフルなら
		{
			rxptr = 0;	// ポインタを先頭に
		}
	}
}
/*----------------------------------------------------------------------------
	ESP-NOWシリアル受信処理
	書式 void RxSensor(BOOL* fsense12, volatile UCHAR* lastsensor);

	BOOL* fsense12;			子機が検知した
	UCHAR* LastSensor;		検知したセンサー番号 1 or 2
----------------------------------------------------------------------------*/
void RxSensor(BOOL* fsense12, volatile UCHAR* lastsensor)
{
	CHAR c, *p;
	SHORT i;
	static CHAR rxbuff[RXMAX];		// シリアル受信バッファ
	static UCHAR rxptr;				// シリアルバッファポインタ

	// 文字を受信していれば
	while (Serial0.available() > 0)
	{
		c = Serial0.read();
		if (rxptr < RXMAX - 2)	// バッファに余裕があるなら
		{
			rxbuff[rxptr++] = c;	// 格納
			if (c == '\n')			// 行末か？
			{
				rxbuff[rxptr] = '\0';	// 文字列をターミネート
				if ((p = strchr(rxbuff, '\r')) != NULL)
				{
					*p = '\0';			// CRを除去
				}
				if (strcmp(rxbuff, ">SENS=1") == 0)
				{
					*lastsensor = 1;	// 子機1が検知
					*fsense12 = TRUE;
				}
				else if (strcmp(rxbuff, ">SENS=2") == 0)
				{
					*lastsensor = 2;	// 子機2が検知
					*fsense12 = TRUE;
				}
				rxptr = 0;	// ポインタを先頭に
			}
		}
		else	// バッファフルなら
		{
			rxptr = 0;	// ポインタを先頭に
		}
	}
}
/*----------------------------------------------------------------------------
	日時をNTPサーバーから取得して時計を合わせる
	書式 void SetClock(EEPstruct* v);

	EEPstruct* v;		LastSetTime最後に時刻調整した時刻
----------------------------------------------------------------------------*/
void SetClock(EEPstruct* v)
{
	CheckWiFi();	// WiFiが切れていたら再接続されるのを待つ

	Println(F("時計を合わせます。"));
	configTzTime("JST-9", "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");
	// 実際に設定されるのを待つ
	time_t t;
	do {
		t = time(NULL);
		delay(100);
    } while (t <= 24 * 3600);	// 時差分を考慮
    v->LastSetTime = t;		// 時刻調整した時間
	Print(F("設定時刻:"));
	Println(StrDate(Val.LastSetTime));
}
/*----------------------------------------------------------------------------
	日時を文字列にして返す
	書式 ret = StrDate(time_t tim);

	CHAR* ret;		日付文字列
	time_t tim;		カレンダー時間
----------------------------------------------------------------------------*/
CHAR* StrDate(time_t tim)
{
	static CHAR str[32];
	time_t t = tim;
	struct tm *tm;

	tm = localtime(&t);
	static const char *wd[7]= {"日","月","火","水","木","金","土"};

	sprintf(str, "%04d/%02d/%02d(%s) %02d:%02d:%02d",
					tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
					wd[tm->tm_wday],
					tm->tm_hour, tm->tm_min, tm->tm_sec);
	return str;
}
/*----------------------------------------------------------------------------
	WiFiに接続する
	書式 void WiFiConnect();
----------------------------------------------------------------------------*/
void WiFiConnect()
{
	BOOL ffind = FALSE;

	WiFi.mode(WIFI_STA);	// ステーションモード
	// BTを使う時はWiFiスリープを生かさないとクラッシュする。
	// しかしスリープ期間はESP-NOWフレームを受けられないのでドロップが発生。
	// ソフトAPモードにする必要がある。(ただしBLEスキャン中はドロップする)
	// →ESP-NOWを外付けにしたのでステーションモードでOK
//	WiFi.mode(WIFI_AP_STA);				// ソフトAPモード
//	WiFi.softAP(Val.HostName, Pass1);	// APは使わないので適当に
	while (!ffind)
	{
		Println(F("WiFiをスキャンします。"));
		WiFi.disconnect();		// いったん状態をクリアして
		SHORT n = WiFi.scanNetworks();
		if (n > 0)
		{
			Print(n);
			Println(F("AP見つかりました。"));
			for (SHORT i = 0; i < n; i++)
			{
				if (WiFi.SSID(i) == Ssid0)
				{
					Println(F("実家のWiFiに接続します。"));
					WiFi.begin(Ssid0, Pass0);	// WiFi開始
					ffind = TRUE;
					break;
				}
				else if (WiFi.SSID(i) == Ssid1)
				{
					Println(F("自宅のWiFiに接続します。"));
					WiFi.begin(Ssid1, Pass1);	// WiFi開始
					fLocHome = TRUE;			// 自宅WiFi
					ffind = TRUE;
					break;
				}
			}
			WiFi.scanDelete();	// スキャン結果を削除してメモリー開放
		}
		if (!ffind)
		{
			Println(F("目的のWiFi APが見つかりません。"));
			delay(1000);
		}
	}
	// WiFi開始を待つ
	WiFiWait();

	// WiFiをスリープさせない設定にする
	// →ESP-NOWを外付けにしたのでスリープしてOK
	// →BLEを組み込む場合にはスリープさせないとクラッシュする！
//	WiFi.setSleep(FALSE);
}
/*----------------------------------------------------------------------------
	WiFi開始を待つ
	時間がたっても接続しない場合、初回はリセット、2度目以降はホットスタート
	書式 void WiFiWait();
----------------------------------------------------------------------------*/
void WiFiWait()
{
	SHORT count = 0;
	static BOOL fconnect;

	while (WiFi.status() != WL_CONNECTED)
	{
		Print(F("."));
		count++;
		if (count % 60 == 0)	// 1分経過
		{
			if (!fconnect)		// 通電後初回なら
			{
				ESP.restart();	// コールドスタート
			}
			else				// 通電後に一度接続済みなら
			{
				HotStart();		// ホットスタート
			}
		}
		// 1秒待つ
		LED_ON
		delay(100);
		LED_OFF
		Mydelay(900);
	}
	fconnect = TRUE;	// 通電後1度は接続した

	Println(F("\nWiFiに接続しました。"));
	Print(F("SSID:"));
	Println(WiFi.SSID());
	Print(F("IP:"));
	Println(WiFi.localIP());
	Print(F("gatewayIP:"));
	Println(WiFi.gatewayIP());
	Print(F("subnetMask:"));
	Println(WiFi.subnetMask());
	Print(F("Channel:"));
	Println(WiFi.channel());
	Print(F("MAC(ST):"));
	Println(WiFi.macAddress());
	Print(F("MAC(AP):"));
	Println(WiFi.softAPmacAddress());
}
/*----------------------------------------------------------------------------
	WiFiが切れていたらWiFi開始を待つ
	書式 void CheckWiFi();
----------------------------------------------------------------------------*/
void CheckWiFi()
{
	if (WiFi.status() != WL_CONNECTED)	// WiFiが切れていたら
	{
		WiFiWait();	// 再接続されるのを待つ
	}
}
/*----------------------------------------------------------------------------
	モーションセンサー割り込み処理(ESPではIRAM_ATTRが必要)
	書式 IRAM_ATTR void SenseInterrupt();
	
	グローバル変数:read
	EEPstruct Val;		fUseCdS CdS使う
	BOOL fBright;		明るさ検知状態
	グローバル変数:write
	BOOL fSens0;		センサー0が検知した
	SHORT LastSensor;	検知したセンサーの番号=0
----------------------------------------------------------------------------*/
IRAM_ATTR void SenseInterrupt()
{
	if (digitalRead(SENSE))	// 立ち上がりでの割り込みだったなら
	{
		// CdS使用フラグがTRUEかつ暗闇ならそもそも検知しなかったことにする
		// (リビングのセンサーはドアを閉めないと？誤検知する？ようなので)
		if (!(Val.fUseCdS && !fBright))
		{
			LED_ON
			fSense0 = TRUE;	// 親機が検知した
			LastSensor = 0;	// 親機のセンサー番号は0
		}
	}
	else
	{
		LED_OFF
	}
}
/*----------------------------------------------------------------------------
	EEPROM書き込み処理
	書式 void EepWrite();

	グローバル変数:write
	EEPstruct Val;	全データ
----------------------------------------------------------------------------*/
void EepWrite()
{
	SHORT i;
	ULONG sum = 0L;
	UCHAR *p;

	p = (UCHAR *)&Val;
	for (i = 0; i < (UCHAR *)&Val.Sum - p - 1; i++)	// sumの手前まで
	{
		sum += *p++;
	}
	Val.Sum = sum;

	EEPROM.put<EEPstruct>(0, Val);
	EEPROM.commit();	// ESPの場合必要

	Println(F("EEPROMに書き込みました。"));
}
/*----------------------------------------------------------------------------
	EEPROM初期データ書き込み
	書式 void EepInitialWrite();

	グローバル変数:write
	EEPstruct Val;	設定値
----------------------------------------------------------------------------*/
void EepInitialWrite()
{
	Println(F("EEPROMに初期データをセットします。"));
	memset(&Val, 0, sizeof(EEPstruct));	// いったんすべてクリア

	// 初期値のあるものはセット
	Val.MailInterval = DEFAULT_MAIL_INTERVAL;
	Val.SleepHour = DEFAULT_SLEEP_HOUR;
	Val.WakeHour = DEFAULT_WAKE_HOUR;
	strcpy(Val.Mail0, DEFAULT_RECIPIENT_EMAIL0);
	strcpy(Val.Mail1, DEFAULT_RECIPIENT_EMAIL1);
	strcpy(Val.Mail2, DEFAULT_RECIPIENT_EMAIL2);
	Val.fUsePhone = TRUE;
	Val.HeatUpTemperature = DEFAULT_HEATUP_TEMP;
	Val.HeatUpTime = DEFAULT_HEATUP_TIME;
	Val.BrightOnTime = DEFAULT_BRIGHT_ON_TIME;
	Val.BrightOffTime = DEFAULT_BRIGHT_OFF_TIME;
	Val.Toffset = DEFAULT_T_OFFSET;
	Val.BrightLevel = DEFAULT_BRIGHT_LEVEL;
	strcpy(Val.DeviceName, DEFAULT_DEVICENAME);
	strcpy(Val.HostName, DEFAULT_HOSTNAME);

	EepWrite();		// EEP書き込み処理
}
/*----------------------------------------------------------------------------
	EEPROM読出し処理
	書式 void EepRead();

	グローバル変数:read&write
	EEPstruct Val;	全データ
----------------------------------------------------------------------------*/
void EepRead()
{
	SHORT i;
	ULONG sum = 0L;
	UCHAR *p;

	if (fInited)	// 通電後初回でないなら
	{
		EepWrite();	// いったん今の値を書き込む
	}

	EEPROM.get<EEPstruct>(0, Val);
	p = (UCHAR *)&Val;
	for (i = 0; i < (UCHAR *)&Val.Sum - p - 1; i++)	// sumの手前まで
	{
		sum += *(p++);
	}

	// チェックサムエラーorデーターが正当でない
	if (sum != Val.Sum ||
		strlen(Val.Mail0) >= 32 || strlen(Val.Mail1) >= 32 ||
		strlen(Val.Mail2) >= 32)
	{
		Println(F("EEPROMエラーのため初期化します。"));
		EepInitialWrite();	// EEP初期データ書き込み
	}
	else if (!fInited)	// 通電後初回なら
	{
		if (Val.fHotStart)	// 再起動したのであれば
		{
			Println(F("*** ホットスタート ***"));
			Val.fHotStart = FALSE;	// ホットスタートであったことをクリア
		}
		else
		{
			// バックアップされている変数値を0クリア
			Println(F("*** コールドスタート ***"));
			Val.LastSetTime = 0;
			Val.NoSenseTimer = 0;
			Val.InLivingTime = 0;
			Val.StatMail = 0;
			Val.RetryCount = 0;
			Val.fWebAccess = FALSE;
			Val.YearZ = 0;
			Val.MonthZ = 0;
			Val.DayZ = 0;
			memset(Val.CountStat, 0, sizeof(Val.CountStat));
			memset(Val.CountPhone, 0, sizeof(Val.CountPhone));
			memset(Val.Temp100, 0, sizeof(Val.Temp100));
			memset(Val.CountBright, 0, sizeof(Val.CountBright));
		}
		EepWrite();		// EEP書き込み処理
	}
	Println(F("EEPROMを読み込みました。"));
}
/*----------------------------------------------------------------------------
	SMTPコールバック
	書式 void smtpCallback(SMTP_Status status);

	SMTP_Status	status	送信ステータス
----------------------------------------------------------------------------*/
void smtpCallback(SMTP_Status status)
{
	/* Print the current status */
	Println(status.info());

	/* Print the sending result */
	if (status.success())
	{
		Println(F("----------------"));
		Print(F("Message sent success: "));
		Println(status.completedCount());
		Print(F("Message sent failed: "));
		Println(status.failedCount());
		Println(F("----------------\n"));

		for (size_t i = 0; i < smtp.sendingResult.size(); i++)
		{
			/* Get the result item */
			SMTP_Result result = smtp.sendingResult.getItem(i);

			Print(F("Message No: "));
			Println(i + 1);
			Print(F("Status: "));
			Println(result.completed ? F("success") : F("failed"));
			Print(F("Date/Time: "));
			Println(MailClient.Time.getDateTimeString(result.timestamp,
							"%B %d, %Y %H:%M:%S"));
			Print(F("Recipient: "));
			Println(result.recipients.c_str());
			Print(F("Subject: "));
			Println(result.subject.c_str());
		}
		Println(F("----------------\n"));

		// You need to clear sending result as the memory usage will grow up.
		smtp.sendingResult.clear();	// ←ESP8266ではクラッシュした
	}
}
/*----------------------------------------------------------------------------
	メール送信
	書式 void SendMail(USHORT mstat);

	USHORT mstat;	メール要因
----------------------------------------------------------------------------*/
void SendMail(USHORT mstat)
{
	String msg;

	if (mstat == 0)
	{
		return;
	}
	CheckWiFi();	// WiFiが切れていたら再接続されるのを待つ

	smtp.debug(1);
	smtp.callback(smtpCallback);

	ESP_Mail_Session session;
	session.server.host_name = SMTP_HOST;
	session.server.port = SMTP_PORT;
	session.login.email = AUTHOR_EMAIL;
	session.login.password = AUTHOR_PASSWORD;
	session.login.user_domain = "";

	SMTP_Message message;
	message.sender.name = fLocHome ? Sname1 : Sname0;
	message.sender.email = AUTHOR_EMAIL;
	String subject = fLocHome ? Sname1 : Sname0;
	subject += " からのお知らせ";
	message.subject = subject;

	if (strchr(Val.Mail0, '@') != NULL)
		message.addRecipient("", Val.Mail0);
	if (strchr(Val.Mail1, '@') != NULL)
		message.addRecipient("", Val.Mail1);
	if (strchr(Val.Mail2, '@') != NULL)
		message.addRecipient("", Val.Mail2);

	if (mstat & MSTAT_NOSENSE)	// センサー検知が途絶えた時のメール
	{
		msg = "最後に";
		msg += SensorName[LastSensor];
		msg += "が、\n";
		msg += StrDate(LastSenseTime);
		msg += "に検知してから\n";
		msg += 	Val.NoSenseTimer / 3600L;
		msg += "時間が過ぎましたが、\n";
		msg += "その後は、どのセンサーにも検知がありません。\n";
	}
	else if (mstat & MSTAT_NOWAKE)	// 朝起きてこない注意のメール
	{
		msg = "いつもの起床時刻から";
		msg += NoWakeCount / 3600;
		msg += "時間が過ぎましたが、\n";
		msg += "今日はまだ、どのセンサーにも検知がありません。\n";
	}
	else if (mstat & MSTAT_RESENSE)	// 再検知のメール
	{
		msg = SensorName[LastSensor];
		msg += "が、\n";
		msg += StrDate(LastSenseTime);
		msg += "に再検知しました。\n";
	}
	else if (mstat & MSTAT_HEATUP)	// 熱中症注意のメール
	{
		msg = "リビングの温度が";
		msg += Val.HeatUpTemperature;
		msg += "℃を超えています。\n今の温度は";
		msg += Temperature;
		msg += "℃です。\n熱中症に注意してください。\n";
	}
	else if (mstat & MSTAT_FIRE)	// 火災感知のメール
	{
		msg = "リビングの温度が";
		msg += FIRE_TEMP;
		msg += "℃を超えています。\n今の温度は";
		msg += Temperature;
		msg += "℃です。\n！！！火災の恐れがあります！！！\n";
	}
	else if (mstat & MSTAT_BRIGHTON)	// 連続点灯注意のメール
	{
		msg = "リビングの照明が点灯したままになっているようです。\n";
	}
	else if (mstat & MSTAT_BRIGHTOFF)	// 連続消灯注意のメール
	{
		msg = "昼間なのにリビングのカーテンが閉じたままか、\n";
		msg += "リビングの照明が消灯したままになっているようです。\n";
	}
	message.text.content = msg;
	message.text.charSet = F("utf-8");
	message.text.transfer_encoding = "base64";
	message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_high;

	if (!smtp.connect(&session))
	{
		return;
	}
	if (!MailClient.sendMail(&smtp, &message ))
	{
		Print(F("Error sending Email, "));
		Println(smtp.errorReason());
		return;
	}
	PrintHeap();			// ヒープ表示
}
/*----------------------------------------------------------------------------
	日付と一日分の統計データをシートに記録
	書式 void WriteSheet(SHORT y, SHORT m, SHORT d, SHORT st, EEPstruct* v);

	SHORT y;		年
	SHORT m;		月
	SHORT d;		日
	SHORT st;		シート番号(0 ～ MAX_SHEET - 1)
	EEPstruct* v;	統計データ
----------------------------------------------------------------------------*/
void WriteSheet(SHORT y, SHORT m, SHORT d, SHORT st, EEPstruct* v)
{
	CHAR str[10];
	SHORT i;
	BOOL fret;
	CHAR workchar[WORKSIZE];	// 文字列ワークエリア

	CheckWiFi();	// WiFiが切れていたら再接続されるのを待つ

	// シートに記録する行
	sprintf(workchar, "%04d/%02d/%02d", y, m, d);
	for (i = 0; i < 24; i++)
	{
		if (st == 5)
		{
			// 明るさ検知回数
			sprintf(str, ",%d", v->CountBright[i]);
		}
		else if (st == 4)
		{
			// 温度
			sprintf(str, ",%3.1f", (FLOAT)v->Temp100[i] / 100.0);
		}
		else if (st == 3)
		{
			// スマホ検知回数
			sprintf(str, ",%d", v->CountPhone[i]);
		}
		else
		{
			// センサ検知回数
			sprintf(str, ",%d", v->CountStat[st][i]);
		}
		MyStrcat(workchar, str);
	}
	Print(F("シート名:"));
	Println(SheetName[st]);
	Println(workchar);
	WriteSheetCore(SheetName[st], workchar);

	PrintHeap();			// ヒープ表示
}
/*----------------------------------------------------------------------------
	GoogleSpreadSheetのシートに書きこむ
	書式 void WriteSheetCore(const CHAR* sheet, const CHAR* val);

	CHAR* sheet;	シート名
	CHAR* val;		書き込むデータ。"セル, 値"とすれば指定セルに値を書く
					データの数が2でない場合は最終行に追加する
----------------------------------------------------------------------------*/
void WriteSheetCore(const CHAR* sheet, const CHAR* val)
{
	SHORT code;
	HTTPClient http;

	Println(F("シートへ書き込みます。"));
	String url = fLocHome ? DeployUrl1 : DeployUrl0;
	url += "?command=write&sheetname=";
	url += urlEncode(sheet);	// シート名
	url += "&val=";
	url += String(val);			// 書き込みデータ

	Print(F("Connecting to "));
	Println(url);

	http.begin(url);		// 接続開始

	code = http.GET();			// レスポンスコード
	Printf("code=%d\n", code);
	while ((code / 10) == 30)	// 300番台ならリダイレクト対応
	{
		url = http.getLocation();	// 次の場所
		http.end();					// 今の場所は終了
		Println(url);
		http.begin(url);			// 次の場所へ行く
		code = http.GET();
		Printf("code=%d\n", code);
	}
	if (code == HTTP_CODE_OK)
	{
		Println(F("書き込み終了。"));
		Println(http.getString());
	}
	else
	{
		Println(F("ファイルが見当たりません。"));
	}
	http.end();
}
/*----------------------------------------------------------------------------
	GoogleSpreadSheetの設定シートの読み取り
	書式 void ReadSheet(BOOL* finit, CHAR* progid, BOOL* fota, EEPstruct* v);

	EEPstruct* v;	各種設定値
	BOOL* finit;	強制初期化要求
	CHAR* progid;	GoogleDriveファイルID
	BOOL* fota;		OTAによるプログラム書き換え要求
----------------------------------------------------------------------------*/
void ReadSheet(BOOL* finit, CHAR* progid, BOOL* fota, EEPstruct* v)
{
	SHORT code, i;
	HTTPClient http;
	CHAR *p;
	BOOL fchng = FALSE;
	CHAR workchar[WORKSIZE];	// 文字列ワークエリア
	workchar[0] = '\0';

	CheckWiFi();	// WiFiが切れていたら再接続されるのを待つ

	Println(F("設定シートを読み取ります。"));
	String url = fLocHome ? DeployUrl1 : DeployUrl0;
	url += "?command=read&sheetname=";
	url += urlEncode("設定");	// シート名
	url += "&val=B1:B17";		// 設定値を記入したカラム

	Print(F("Connecting to "));
	Println(url);

	http.begin(url);			// 接続開始

	code = http.GET();			// レスポンスコード
	Printf("code=%d\n", code);
	while ((code / 10) == 30)	// 300番台ならリダイレクト対応
	{
		url = http.getLocation();	// 次の場所
		http.end();					// 今の場所は終了
		Println(url);
		http.begin(url);			// 次の場所へ行く
		code = http.GET();
		Printf("code=%d\n", code);
	}
	// データ要求
	if (code == HTTP_CODE_OK)
	{
		// 設定値(カンマ区切りデータ)
		url = http.getString();
		if (url.length() < WORKSIZE - 1)
		{
			strcpy(workchar, url.c_str());	// いったんCHAR型に
		}
		// データの最後の行だと改行がくっついてくるので除去
		CHAR* p;
		if ((p = strchr(workchar, '\r')) != NULL) *p = '\0';
		if ((p = strchr(workchar, '\n')) != NULL) *p = '\0';
		Println(workchar);
	}
	else
	{
		Println(F("ファイルが見当たりません。"));
		workchar[0] = '\0';
	}
	http.end();

	if (strlen(workchar))
	{
		p = strtokComma(workchar);
		i = atoi(p);
		if (i > 0 && i <= 24 && i != v->MailInterval)
		{
			v->MailInterval = i;	// 検出間隔[時間]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 24 && i != v->SleepHour)
		{
			v->SleepHour = i;		// 就寝時刻[時]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 24 && i != v->WakeHour)
		{
			v->WakeHour = i;		// 起床時刻[時]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		if (strcmp(p, v->Mail0) != 0 && strlen(p) < 32)
		{
			strcpy(v->Mail0, p);	// メール0
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		if (strcmp(p, v->Mail1) != 0 && strlen(p) < 32)
		{
			strcpy(v->Mail1, p);	// メール1
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		if (strcmp(p, v->Mail2) != 0 && strlen(p) < 32)
		{
			strcpy(v->Mail2, p);	// メール2
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i == 1)
		{
			*finit = TRUE;	// 強制初期化要求
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 1 && (BOOL)i != v->fUseCdS)
		{
			v->fUseCdS = (BOOL)i;	// CdSを使う/使わない
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 1 && (BOOL)i != v->fUsePhone)
		{
			v->fUsePhone = (BOOL)i;	// スマホの存在を使う/使わない
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 40 && i != v->HeatUpTemperature)
		{
			v->HeatUpTemperature = i;	// 熱中症警報温度[℃]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 240 && i != v->HeatUpTime)
		{
			v->HeatUpTime = i;			// 熱中症警報時間[分]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 240 && i != v->BrightOnTime)
		{
			v->BrightOnTime = i;		// 連続点灯注意時間[時]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 240 && i != v->BrightOffTime)
		{
			v->BrightOffTime = i;		// 連続消灯注意時間[時]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		FLOAT f = atof(p);
		if (f != v->Toffset)
		{
			v->Toffset = f;			// 温度センサーオフセット[℃]
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 4095 && i != v->BrightLevel)
		{
			v->BrightLevel = i;		// 明るさ判定A/D値
			fchng = TRUE;
		}
		p = strtokComma(NULL);
		if (strlen(p) < 64)
		{
			strcpy(progid, p);	// 書き換えバイナリーファイルID
		}
		p = strtokComma(NULL);
		i = atoi(p);
		if (i >= 0 && i <= 1)
		{
			*fota = (BOOL)i;		// OTA書き換え要求
		}
		if (fchng)	// 変更があれば
		{
			EepWrite();	// EEPROM書き換え
		}
	}
	Print(F("MailInterval[時間]=")), Println(v->MailInterval);
	Print(F("SleepHour[時]=")), Println(v->SleepHour);
	Print(F("WakeHour[時]=")), Println(v->WakeHour);
	Print(F("Mail0=")), Println(v->Mail0);
	Print(F("Mail1=")), Println(v->Mail1);
	Print(F("Mail2=")), Println(v->Mail2);
	Print(F("fReqInit=")), Println(*finit);
	Print(F("fUseCdS=")), Println(v->fUseCdS);
	Print(F("fUsePhone=")), Println(v->fUsePhone);
	Print(F("HeatUpTemperature[℃]=")), Println(v->HeatUpTemperature);
	Print(F("HeatUpTime[分]=")), Println(v->HeatUpTime);
	Print(F("BrightOnTime[時]=")), Println(v->BrightOnTime);
	Print(F("BrightOffTime[時]=")), Println(v->BrightOffTime);
	Print(F("Toffset[℃]=")), Println(v->Toffset);
	Print(F("BrightLevel=")), Println(v->BrightLevel);
	Print(F("ProgramId=")), Println(progid);
	Print(F("fReqOTA=")), Println(*fota);
}
/*----------------------------------------------------------------------------
	GoogleSpreadSheetの統計データシートへの書き込みと設定シート読み取り
	書式 void SheetAccess(SHORT y, SHORT m, SHORT d, BOOL fc, EEPstruct* v);
	
	SHORT y;		年
	SHORT m;		月
	SHORT d;		日
	BOOL fc;		統計データクリア要求
	EEPstruct* v;	統計データ
----------------------------------------------------------------------------*/
void SheetAccess(SHORT y, SHORT m, SHORT d, BOOL fc, EEPstruct* v)
{
	// GoogleSpreadSheetに各統計データを書く
	for (SHORT sheet = 0; sheet < MAX_SHEET; sheet++)
	{
		WriteSheet(y, m, d, sheet, v);
	}
	if (fc)	// 統計データクリア要求？
	{
		// 統計データクリア
		memset(v->CountStat, 0, sizeof(v->CountStat));
		memset(v->CountPhone, 0, sizeof(v->CountPhone));
		memset(v->Temp100, 0, sizeof(v->Temp100));
		memset(v->CountBright, 0, sizeof(v->CountBright));
	}

	// GoogleSpreadSheetから設定パラメータを読む
	ReadSheet(&fReqInit, ProgramId, &fReqOTA, &Val);
}
/*----------------------------------------------------------------------------
	GoogleDriveのファイルを使ってプログラムOTAアップデート
	書式 void OTAupdate(CHAR* id);

	CHAR* id;	GoogleDriveのファイルID。↓ファイルのリンクの
				https://drive.google.com/file/d/～/view?usp=drive_linkの間部分
----------------------------------------------------------------------------*/
const String ProgramUrl = "https://drive.google.com/uc?id=";
void OTAupdate(CHAR* id)
{
	HTTPClient http;
	SHORT code;

	String url =  ProgramUrl + String(id);

	Print(F("Connecting to "));
	Println(url);

	http.begin(url);			// 接続開始

	code = http.GET();			// レスポンスコード
	Printf("code=%d\n", code);
	while ((code / 10) == 30)	// 300番台ならリダイレクト対応
	{
		url = http.getLocation();	// 次の場所
		http.end();					// 今の場所は終了
		Println(url);
		http.begin(url);			// 次の場所へ行く
		code = http.GET();			// レスポンスコード
		Printf("code=%d\n", code);
	}
	// データ要求
	if (code == HTTP_CODE_OK)		// ファイルがあるなら
	{
		WiFiClient* stream = http.getStreamPtr();	// ストリーム取得
		size_t total = http.getSize();				// トータルサイズ
		Print(F("total="));
		Println(total);
		if (!Update.begin())		// アップデート開始
		{
			Println(F("サイズが大きすぎます。"));
			http.end();
			return;
		}
		Println(F("Update.begin"));
		size_t remain, receive, read;
		SHORT tim = 0, pct = 0, pctz = 0;
		UCHAR buf[128];
		remain = total;		// 未処理のデータ数
		while (remain > 0)	// 未処理のデータがある間
		{
			receive = stream->available();	// 受け取っているデータ長
			if (receive)	// データを受けているなら
			{
				// 最大でもバッファ分のデータを受け取る
				read = stream->readBytes(buf,
						((receive > sizeof(buf)) ? sizeof(buf) : receive));
				remain -= read;		// 未処理データの数を減らす
				Update.write(buf, read);	// 受け取ったデータ分アップデート
				// プログレス表示(ESP8266ではUpdate.onProgressに飛んでこない)
				pct = (SHORT)(100L * (total - remain) / total);
				if (pct != pctz)
				{
					Print(F("OTA Progress: "));
					Print(pct);
					Println(F("%"));
					pctz = pct;
				}
				tim = 0;
			}
			else	// データが来るまで待つ
			{
				delay(100);
				if (++tim > 600 || !http.connected())	// 1分経ったか切断？
				{
					Println(F("Updateタイムアウト"));	// 中断して再起動
					HotStart();	// ホットスタート
				}
			}
		}
		Update.end(TRUE);	// アップデート終了
		Println(F("Update.end"));
	}
	else
	{
		Println(F("ファイルが見当たりません。"));
	}
	http.end();
}
/*----------------------------------------------------------------------------
	','カンマで区切られた文字列の手前部分を返す(最大STRBUFMAX-1文字)
	書式 ret = strtokComma(CHAR* s);

	CHAR* ret;	抽出文字列(存在しなければNULLを返す)
	CHAR* s;	入力文字列(2回目以降はNULLを引数にする)
----------------------------------------------------------------------------*/
#define STRBUFMAX 64
CHAR* strtokComma(CHAR* s)
{
	static CHAR* p0;
	static CHAR buf[STRBUFMAX];
	CHAR* p;
	SHORT len;

	if (s)
	{
		p0 = s;	// 初期の文字の先頭
	}
	if (*p0 == '\0')	// 最後まで行っていたら
	{
		return NULL;	// NULLを返す
	}
	p = strchr(p0, ',');	// ','の位置
	if (p)	// 発見した？
	{
		len = p - p0;		// ','手前までの文字数
		if (len < STRBUFMAX)// バッファに入るなら
		{
			strncpy(buf, p0, len);	// 文字をコピー
			*(buf + len) = '\0';	// ターミネート
		}
		else
		{
			strcpy(buf, "#over#");	// エラー
		}
		p0 = p + 1;	// ','の次の位置を次回先頭に
	}
	else	// 発見できなかった→最後の文字列だった
	{
		len = strlen(p0);
		p0 += len;		// 文字列の最後の'\0'をポイント
		return p0 - len;// 文字列をそのまま返す
	}
	return buf;	// 抽出した文字列を返す
}
/*----------------------------------------------------------------------------
	文字列の連結(最大でWORKSIZE)
	書式 void MyStrcat(CHAR* d, CHAR* s);

	CHAR* d;	コピー先
	CHAR* s;	連結する文字列
----------------------------------------------------------------------------*/
void MyStrcat(CHAR* d, CHAR* s)
{
	if (strlen(d) + strlen(s) < WORKSIZE - 1)
	{
		strcat(d, s);
	}
}
/*----------------------------------------------------------------------------
	文字列をURLエンコード
	書式 ret = urlEncode(const CHAR *msg);

	Styring ret;	変換された文字
	CHAR* msg;		変換する文字列
----------------------------------------------------------------------------*/
String encodedMsg(50, 0);
String urlEncode(const CHAR *msg)
{
	const CHAR *hex = "0123456789ABCDEF";

	encodedMsg = "";
	while (*msg != '\0')
	{
		if (('a' <= *msg && *msg <= 'z') || ('A' <= *msg && *msg <= 'Z') ||
			('0' <= *msg && *msg <= '9') || *msg == '-' || *msg == '_' ||
			*msg == '.' || *msg == '~')
		{
			encodedMsg += *msg;
		}
		else
		{
			encodedMsg += '%';
			encodedMsg += hex[(unsigned char)*msg >> 4];
			encodedMsg += hex[*msg & 0xf];
		}
		msg++;
	}
	return encodedMsg;
}
/*----------------------------------------------------------------------------
	フラグのオン時間の累積
	書式 void AddOnTime(BOOL flag, SHORT* ontime)

	BOOL flag;		フラグ
	SHORT* ontime;	オン時間
----------------------------------------------------------------------------*/
#define	TIMEMAX 30000
void AddOnTime(BOOL flag, SHORT* ontime)
{
	if (flag)							// オンしてるなら
	{
		if (*ontime < TIMEMAX)
		{
			(*ontime)++;				// オン時間＋＋
		}
	}
	else
	{
		*ontime = 0;
	}
}
/*----------------------------------------------------------------------------
	平均値の算出
	書式 ret = Average(FLOAT dat, BOOL finit);

	FLOAT ret;	平均値
	FLOAT dat;	入力値
	BOOL finit;	初期化要求
----------------------------------------------------------------------------*/
FLOAT Average(FLOAT dat, BOOL finit)
{
	static FLOAT total, ret;
	static SHORT n;
	
	if (n == 0)		// 初回？(今の値を返す)
	{
		total = ret = dat;	// 今の値で初期化
		n = 1;
	}
	if (finit)		// 初期化要求？	(前回までの平均値を返す)
	{
		total = dat;	// 今の値で初期化(戻り値は前回値)
		n = 1;
	}
	else
	{
		total += dat;
		n++;
		ret = total / (FLOAT)n;	// 平均値を計算
	}
	return ret;
}
/*----------------------------------------------------------------------------
	map関数FLOAT版(最大最少をリミット)
	書式 ret = mapf(FLOAT x,
					FLOAT in_min, FLOAT in_max,
					FLOAT out_min, FLOAT out_max)

	FLOAT ret;		出力
	FLOAT x;		入力
	FLOAT in_min;	入力最小値
	FLOAT in_max;	入力最大値
	FLOAT out_min;	出力最小値
	FLOAT out_max;	出力最大値  out_min < out_maxであること
----------------------------------------------------------------------------*/
FLOAT mapf(FLOAT x, FLOAT in_min, FLOAT in_max, FLOAT out_min, FLOAT out_max)
{
	FLOAT ret;

	ret = (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
	ret = constrain(ret, out_min, out_max);
	return ret;
}
/*----------------------------------------------------------------------------
	Xiaomi LYWSD03MMC アドバタイズ受信時のコールバック

	グローバル変数:read
	CHAR Val.DeviceName[16];
	グローバル変数:write
	FLOAT TemperatureExt;	温度[℃]
	FLOAT HumidityExt;		湿度[%]
	UCHAR BattExt;			バッテリー残量[%]
	BOOL fRxExtTemp;
	NoRxTempCount;
----------------------------------------------------------------------------*/
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks
{
	// スキャン結果受信
	void onResult(BLEAdvertisedDevice advertisedDevice)
	{
		// 温度計のデバイス名
		std::string strDeviceName(Val.DeviceName);

		// 一致した？
		if (strDeviceName == advertisedDevice.getName())

		{ 
			// カスタムファームpvvxの仕様に従って温度計のデータを取得
			uint8_t *pPayload = advertisedDevice.getPayload();
			size_t len = advertisedDevice.getPayloadLength();

			SHORT temp = *(pPayload + 13) | *(pPayload + 14) << 8;
			USHORT humi = *(pPayload + 15) | *(pPayload + 16) << 8;
			USHORT mvolt = *(pPayload + 17) | *(pPayload + 18) << 8;
			UCHAR vlevel = *(pPayload + 19);

			TemperatureExt = (FLOAT)temp / 100.0;	// 温度[℃]
			HumidityExt = (FLOAT)humi / 100.0;		// 湿度[%]
			//BattExt = vlevel;						// バッテリー残量[%]
			// バッテリー残量[%](3.0Vを100%とする)
			if (mvolt > 0)
			{
				BattExt = (UCHAR)(100.0 * 3000.0 / (FLOAT)mvolt);
			}
			pBLEScan->stop();

			fRxExtTemp = TRUE;		// 外部温度計から受信
			NoRxTempCount = 0;		// 外部温度計未受信カウンタクリア
		}
	}
};
/*----------------------------------------------------------------------------
	BLEスキャン終了時のコールバック
----------------------------------------------------------------------------*/
static void scanCompleteCB(BLEScanResults scanResults)
{
}

/*----------------------------------------------------------------------------
	BLEイニシャライズ
----------------------------------------------------------------------------*/
void InitBLE()
{
	BLEDevice::init("");
	pBLEScan = BLEDevice::getScan();
	pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
	pBLEScan->setActiveScan(true); // アクティブスキャン
	pBLEScan->setInterval(100);
	pBLEScan->setWindow(99);
}
/*** end of "mimamori4.ino" ***/

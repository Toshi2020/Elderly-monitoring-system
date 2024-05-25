/*****************************************************************************
*
*	mimamori4_sensor.ino -- 見守りセンサー 4X(子機) WEMOS D1 mini
*
*	・モーションセンサーの検知をESP-NOWで送信
*
*	rev1.0	2024/01/12	initial revision by	Toshi
*	rev1.1	2024/01/23	OTA対応
*
*****************************************************************************/
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

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

// EEPROMデフォルト値
#define SENSOR_NUMBER 1		// センサー番号
#define HOSTNAME "mimamori"	// WiFiホスト名

/********* WEMOS D1 mini(ESP8266) ピン設定 *********/
#define SENSE D1
#define LED D2
#define LED_ON  digitalWrite(LED, 1);
#define LED_OFF digitalWrite(LED, 0);

#define ACK 0

/********* グローバル変数 *********/
UCHAR SensorNum;	// センサー番号1,2
const String Ssid0 = "SSID_JIKKA";		// WiFi SSID(実家)
const String Ssid1 = "SSID_JITAKU";		// WiFi SSID(自宅)
const String Pass0 = "pass_jikka";		// WiFi key(実家)
const String Pass1 = "pass_jitaku";		// WiFi key(自宅)
BOOL fLocHome;			// 自宅ならTRUE
#define RXMAX 80		// シリアル受信バッファサイズ
CHAR RxBuff[RXMAX];		// シリアル受信バッファ
CHAR RxPtr;				// バッファポインタ
volatile BOOL fSense;	// 検知した

// ESP-NOW用MACアドレス
UCHAR MacAddr[6] = {0xff,0xff,0xff,0xff,0xff,0xff};	// 初回はブロードキャスト
const UCHAR MacAddrBroad[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
UCHAR SendData[2];
BOOL fAck;

// WiFiサーバー
#define SERVER_PORT 54321
WiFiServer server(SERVER_PORT);
WiFiClient Client;

/********* プロトタイプ宣言 *********/
IRAM_ATTR void SenseInterrupt();
void SetClock();
void WiFiConnect();
void WiFiWait();
CHAR* StrDate(time_t tim);
void EepWrite();
void EepInitialWrite();
void EepRead();
void RxCmd();
void OnDataSent(UCHAR *mac, UCHAR status);
void OnDataRecv(UCHAR *mac, UCHAR *data, UCHAR data_len);
void Mydelay(ULONG tim);
void PrintHeap();
void Wait1sec(BOOL f1, BOOL f2);
void AddOnTime(BOOL flag, SHORT* ontime);

/*----------------------------------------------------------------------------
	セットアップ
----------------------------------------------------------------------------*/
void setup()
{
	CHAR myname[32];

	//pinMode(SENSE, INPUT);
	pinMode(LED, OUTPUT);
	LED_OFF

	delay(2000);	// ブート時に74880bpsでデータが来るので

	// ハードウェアシリアル
	Serial.begin(115200);
	Println(F("*** reset ***"));

	// EEPROMからデータを読み出す
    EEPROM.begin(1024); // ESP8266の場合サイズ初期化が必要
	EepRead();

	// WiFi開始
	WiFiConnect();

	// WiFi切断時に自動復帰
	WiFi.setAutoReconnect(TRUE);

	// WiFiサーバー開始
	server.begin();

	// ESP-NOWイニシャライズ
	if (esp_now_init() == 0)
	{
		Println(F("ESPNow Init Success"));
	}
	else
	{
		Println(F("ESPNow Init Failed"));
		ESP.restart();
	}
	// ESPがステーションモードならESP-NOWの役割はコントローラーとする
	esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
	// ESP-NOWコールバック登録
	esp_now_register_send_cb(OnDataSent);
	esp_now_register_recv_cb(OnDataRecv);

	// WiFi OTA設定
	// (MDNS.beginがArduinoOTAの内部で行われる)
	sprintf(myname, "%s_%d", HOSTNAME, SensorNum);
	ArduinoOTA.setHostname(myname);
	ArduinoOTA.setPassword(fLocHome ? Pass1.c_str() : Pass0.c_str());
	ArduinoOTA.begin();

	// WiFi OTA状況の表示
	ArduinoOTA.onStart([]() {
		Println(F("OTA Start."));
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
	Print(myname);
	Print(F(".local ポート="));
	Println(SERVER_PORT);

	// モーションセンサー割り込み開始
	attachInterrupt(SENSE, SenseInterrupt, CHANGE);

	// ヒープ表示
	PrintHeap();
}
/*----------------------------------------------------------------------------
	メインループ
----------------------------------------------------------------------------*/
void loop()
{
	static time_t tz;
	struct tm *tm;
	static USHORT stat, statz;
	static SHORT settimecount;
	static SHORT wifiofftime;

	/************************************************************************/
	// モーションセンサーが検知した
	if (fSense)
	{
		fSense = FALSE;
		SendData[0] = SensorNum;		// センサー番号1or2
		SendData[1]++;					// パケットカウント++

		for (SHORT i = 0; i < 30; i++)	// ACK受信まで送信をリトライ
		{
			// ESP-NOWで送信(初回はブロードキャスト)
			USHORT err = esp_now_send(MacAddr, SendData, 2);

			// 約2s間ACK待ち
			for (SHORT j = 0; j < 19 + SensorNum; j++)
			{
				Mydelay(100);
				if (fAck)	// ACKを受信したら
				{
					Println(F("receive ACK"));
					break;	// ACK待ちを抜ける
				}
			}
			if (fAck)	// ACKを受信したら
			{
				fAck = FALSE;
				break;	// リトライのループを抜ける
			}
		}
		// ヒープ表示
		PrintHeap();
	}

	/************************************************************************/
	// その他の処理
	//

	// WiFiステータスの変化を表示
	stat = WiFi.status();
	if (stat != statz) Printf("WiFi.status = %d\n", stat);
	statz = stat;

	// フェールセーフ：WiFiオフが長時間なら再接続
	AddOnTime(stat != WL_CONNECTED, &wifiofftime);	// WiFiオフ時間[s]
	if (wifiofftime >= 300)	// 5分以上自動で再接続しなかった？
	{
		wifiofftime = 0;
		WiFiConnect();		// 再度接続を試みる
	}

	// 1秒待つ
	Wait1sec(WiFi.status() != WL_CONNECTED, FALSE);
}
/*----------------------------------------------------------------------------
	ヒープ表示
----------------------------------------------------------------------------*/
size_t GetHeapSize()
{
	return system_get_free_heap_size();
}
void PrintHeap()
{
	Print(F("フリーヒープサイズ:"));
	Println(GetHeapSize());
}
/*----------------------------------------------------------------------------
	1秒待つ
	書式 void Wait1sec(BOOL f1, BOOL f2);

	BOOL f1;	WiFi非接続
	BOOL f2;	子機が検知した
----------------------------------------------------------------------------*/
void Wait1sec(BOOL f1, BOOL f2)
{
	if (f1)	// WiFi非接続
	{
		LED_ON
		Mydelay(100);
		LED_OFF
		Mydelay(900);
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
		Mydelay(700);
	}
	else
	{
		Mydelay(1000);
	}
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
	USBシリアルおよびWiFiシリアルコマンド受信処理
	書式 void RxCmd();
----------------------------------------------------------------------------*/
void RxCmd()
{
	CHAR c, cmd, *p, num, *pos;
	SHORT i;

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
		if (RxPtr < RXMAX - 2)	// バッファに余裕があるなら
		{
			RxBuff[RxPtr++] = c;	// 格納
			if (c == '\n')			// 行末か？
			{
				RxBuff[RxPtr] = '\0';	// 文字列をターミネート
				if ((pos = strchr(RxBuff, '\r')) != NULL)
				{
					*pos = '\0';		// CRを除去
				}
				if (RxBuff[0] == '-')	// コマンド行か？
				{
					cmd = RxBuff[1];	// コマンド文字
					p = &RxBuff[2];
					while (*p > 0 && *p <= ' ') p++;// 先頭の空白除去
					switch (cmd)
					{
						// EEPデフォルトに
						case 'c':
							EepInitialWrite();
							break;
						// センサー番号
						case 'b':
							i = atoi(p);
							if (i >= 1 && i <= 2)
							{
								SensorNum = i;
							}
							break;
						default:
							break;
					}
					// EEPROM内容の変更なら
					if (cmd == 'b' || cmd == 'c')
					{
						EepWrite();		// EEPROM書き込み
						EepRead();		// EEPROM読み出し
						Println(F("再起動します。"));
						ESP.restart();	// ESP再起動
					}
				}
				else if (RxBuff[0] == '?')
				{
					Print(F("IP:"));
					Println(WiFi.localIP());
					Print(F("Channel:"));
					Println(WiFi.channel());
					Print(F("MAC(ST):"));
					Println(WiFi.macAddress());
					PrintHeap();	// ヒープ表示
					Println(F("*** 設定コマンドと現在値 ***"));
					Print(F("-b センサー番号[1,2] ")); Println(SensorNum);
					Println(F("-c EEPROMイニシャライズ"));
					EepRead();		// EEPROM読み出し
				}
				RxPtr = 0;	// ポインタを先頭に
			}
		}
		else	// バッファフルなら
		{
			RxPtr = 0;	// ポインタを先頭に
		}
	}
}
/*----------------------------------------------------------------------------
	WiFi接続
----------------------------------------------------------------------------*/
void WiFiConnect()
{
	BOOL ffind = FALSE;

	WiFi.mode(WIFI_STA);	// ステーションモードで
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

	// WiFiをスリープさせない(常時受信しないのなら不要か？)
//	WiFi.setSleepMode(WIFI_NONE_SLEEP);
}
/*----------------------------------------------------------------------------
	WiFi開始を待つ
----------------------------------------------------------------------------*/
void WiFiWait()
{
	SHORT count = 0;

	while (WiFi.status() != WL_CONNECTED)
	{
		Print(".");
		count++;
		if (count % 60 == 0)	// 1分経過
		{
			ESP.restart();		// ESP再起動
		}
		// 1秒待つ
		LED_ON
		Mydelay(100);
		LED_OFF
		Mydelay(900);
	}

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
	モーションセンサー割り込み処理(ESP8266ではIRAM_ATTRが必要)
----------------------------------------------------------------------------*/
IRAM_ATTR void SenseInterrupt()
{
	if (digitalRead(SENSE))	// 立ち上がりでの割り込みだったなら
	{
		LED_ON
		fSense = TRUE;
	}
	else
	{
		LED_OFF
	}
}
/*----------------------------------------------------------------------------
	ESP-NOW 送信コールバック
----------------------------------------------------------------------------*/
void OnDataSent(UCHAR *mac, UCHAR status)
{
	CHAR macStr[18];

	snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	Print(F("Sent to: "));
	Print(macStr);
	Print(F(" ("));
	Print(SendData[1]);
	Println(F(")"));
}

/*----------------------------------------------------------------------------
	ESP-NOW受信コールバック
----------------------------------------------------------------------------*/
void OnDataRecv(UCHAR *mac, UCHAR *data, UCHAR data_len)
{
	CHAR macStr[18];

	snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	Print(F("Recv from: "));
	Print(macStr);
	Print(F(" ("));
	Print(data[0]);
	Println(F(")"));

	if (data_len == 1 && data[0] == ACK)	// ACK?(親機がESP8266)
	{
		memcpy(MacAddr, mac, 6);	// 次回送信先は親機のmacアドレス
		fAck = TRUE;				// ACK受信
	}

	// 子機がESP8266で親機がESP32だとブロードキャスト以外受けられないようだ
	// →ESP32が親機の場合、ACKを2つ送るようにした

	// ACK?(親機がESP32)
	else if (data_len == 2 && data[0] == ACK && data[1] == ACK)
	{
		memcpy(MacAddr, MacAddrBroad, 6);	// 次回送信先もブロードキャスト
		fAck = TRUE;						// ACK受信
	}
}
/*----------------------------------------------------------------------------
	EEP操作
----------------------------------------------------------------------------*/
// EEPROM構造体
struct EEPstruct
{
	UCHAR sensor;
	UCHAR dmy;
	ULONG sum;
} EEP;
/*----------------------------------------------------------------------------
	EEP書き込み処理
----------------------------------------------------------------------------*/
void EepWrite()
{
	SHORT i;
	ULONG sum = 0L;
	UCHAR *p;

	memset(&EEP, 0, sizeof(EEP));
	EEP.sensor = SensorNum;

	p = (UCHAR *)&EEP;
	for (i = 0; i < (UCHAR *)&EEP.sum - p - 1; i++)	// sumの手前まで
	{
		sum += *p++;
	}
	EEP.sum = sum;

	EEPROM.put<EEPstruct>(0, EEP);
	EEPROM.commit();	// ESP8266の場合必要

	Println(F("EEPROMに書き込みました。"));
}
// EEP初期データ書き込み
void EepInitialWrite()
{
	Println(F("EEPROMに初期データをセットします。"));
	SensorNum = SENSOR_NUMBER;

	EepWrite();
}
/*----------------------------------------------------------------------------
	EEP読出し処理
----------------------------------------------------------------------------*/
void EepRead()
{
	SHORT i;
	ULONG sum = 0L;
	UCHAR *p;

	memset(&EEP, 0, sizeof(EEP));
	EEPROM.get<EEPstruct>(0, EEP);
	p = (UCHAR *)&EEP;
	for (i = 0; i < (UCHAR *)&EEP.sum - p - 1; i++)	// sumの手前まで
	{
		sum += *(p++);
	}

	// チェックサムエラーorデーターがおかしい
	if (sum != EEP.sum || EEP.sensor < 1 || EEP.sensor > 2)
	{
		Println(F("EEPROMエラーのため初期化します。"));
		EepInitialWrite();	// EEP初期データ書き込み
	}
	SensorNum = EEP.sensor;

	Println(F("EEPROM読み出しデータ"));
	Print(F("-b "));
	Println(SensorNum);
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
	if (flag)							/* オンしてるなら */
	{
		if (*ontime < TIMEMAX)
		{
			(*ontime)++;				/*オン時間＋＋ */
		}
	}
	else
	{
		*ontime = 0;
	}
}
/*** end of "mimamori4_sensor.ino" ***/

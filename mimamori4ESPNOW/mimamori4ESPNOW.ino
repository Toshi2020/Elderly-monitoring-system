/*****************************************************************************
*
*	mimamoriESPNOW.ino -- 見守りセンサー ESPNOW受信サブボード for ESP-01
*
*	・モーションセンサーの検知をESP-NOWで受信してシリアル送信
*
*	rev1.0	2024/02/08	initial revision by	Toshi
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

#define HOSTNAME "mimamoriESPNOW"	// WiFiホスト名
#define ACK 0

/********* グローバル変数 *********/
const String Ssid0 = "SSID_JIKKA";		// WiFi SSID(実家)
const String Ssid1 = "SSID_JITAKU";		// WiFi SSID(自宅)
const String Pass0 = "pass_jikka";		// WiFi key(実家)
const String Pass1 = "pass_jitaku";		// WiFi key(自宅)
BOOL fLocHome;			// 自宅ならTRUE
BOOL fRecv12;			// 子機から受信した
BOOL fSense12;			// 子機が検知した
UCHAR LastSensor;		// 最後に検知したセンサー番号1,2

// ESP-NOW用MACアドレス
UCHAR MacAddr[6] = {0xff,0xff,0xff,0xff,0xff,0xff};	// 初回ブロードキャスト
UCHAR SendData[2];
BOOL fAck;

// WiFiサーバー
#define SERVER_PORT 54321
WiFiServer server(SERVER_PORT);
WiFiClient Client;

/********* プロトタイプ宣言 *********/
void PrintHeap();
void WiFiConnect();
void WiFiWait();
void OnDataSent(UCHAR *mac, UCHAR status);
void OnDataRecv(UCHAR *mac, UCHAR *data, UCHAR data_len);

/*----------------------------------------------------------------------------
	セットアップ
----------------------------------------------------------------------------*/
void setup()
{
	delay(2000);	// ブート時に74880bpsでデータが来るので

	// ハードウェアシリアル
	Serial.begin(115200);
	Println(F("*** reset ***"));

	WiFi.mode(WIFI_STA);	// ステーションモードで
	WiFi.disconnect();		// いったん状態をクリア

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

	// WiFi開始
	WiFiConnect();

	// WiFi切断時に自動復帰
	WiFi.setAutoReconnect(TRUE);

	// WiFiサーバー開始
	server.begin();

	// WiFi OTA設定
	// (MDNS.beginがArduinoOTAの内部で行われる)
	ArduinoOTA.setHostname(HOSTNAME);
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
	Print(HOSTNAME);
	Print(F(".local ポート="));
	Println(SERVER_PORT);
	
	// ヒープ表示
	PrintHeap();
}
/*----------------------------------------------------------------------------
	メインループ
----------------------------------------------------------------------------*/
void loop()
{
	UCHAR send[1];
	static ULONG t0;

	// 子機から受信したならACKを返す
	if (fRecv12)
	{
		fRecv12 = FALSE;	// 受信フラグを降ろす
		if (LastSensor == 1 || LastSensor == 2)	// 子機からの受信なら
		{
			send[0] = ACK;
			esp_now_send(MacAddr, send, 1);	// ACK送信(1バイト)
		}
	}

	// 子機のモーションセンサが検知した？
	if (fSense12)
	{
		fSense12 = FALSE;	// 検知フラグを降ろす
		if (LastSensor == 1)
		{
			Println(F(">SENS=1"));
		}
		else if (LastSensor == 2)
		{
			Println(F(">SENS=2"));
		}
	}

	ArduinoOTA.handle();	// WiFi OTA

	// WiFiが接続中なら
	if (WiFi.status() == WL_CONNECTED)
	{
		t0 = millis();	// WiFiがオンの時刻

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
	else
	{
		// フェールセーフ：WiFiオフが長時間なら再接続
		if (t0 != 0L && millis() >= t0 + 600000L)	// WiFiオフが5分以上？
		{
			t0 = 0L;
			WiFiConnect();		// 再度接続を試みる
		}
	}
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

	// WiFiをスリープさせない
	// ステーションモードでWiFiをコネクトするとWiFiスリープが有効になる
	// ESPNOWを常時受信するためにはスリープさせない設定が必要となる
	WiFi.setSleepMode(WIFI_NONE_SLEEP);
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
		delay(1000);
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
	ESP-NOW 送信コールバック
----------------------------------------------------------------------------*/
void OnDataSent(UCHAR *mac, UCHAR status)
{
	CHAR macStr[18];

	snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	Print(F("Sent to: "));
	Println(macStr);
}

/*----------------------------------------------------------------------------
	ESP-NOW受信コールバック
----------------------------------------------------------------------------*/
void OnDataRecv(UCHAR *mac, UCHAR *data, UCHAR data_len)
{
	CHAR macStr[18];
	UCHAR sensor, packet;
	static UCHAR pcount[3];

	snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	Print(F("Recv from: "));
	Println(macStr);
	sensor = data[0];
	packet = data[1];
	if (data_len == 2 && (sensor == 1 || sensor == 2))	// 子機からか？
	{
		memcpy(MacAddr, mac, 6);		// 送信元のmacアドレスをコピー
		if (pcount[sensor] != packet)	// 前回の受信と違うカウント値なら
		{
			fSense12 = TRUE;	// 子機が検知した
			LastSensor = sensor;
			pcount[sensor] = packet;
		}
		fRecv12 = TRUE;			// 子機から受信した
	}
}
/*** end of "mimamoriESPNOW.ino" ***/

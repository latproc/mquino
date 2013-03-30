#include <Arduino.h>
#include <EEPROM.h>
#include <Ethernet.h>
#include <Dns.h>
//#define DEBUG_CONSOLE
//#define DEBUG 1
#define FEEDBACK
#define USEMQTT 1
#ifdef USEMQTT
#include <SPI.h>
#include <IPAddress.h>
#include <PubSubClient.h>
#endif
struct ProgramSettings
{
  byte header[2];
  char hostname[40];
  byte ip[4];
  byte mac_address[6];
  char broker_host[40];
  int broker_port;
  byte broker_ip[4];
  IPAddress dns_address;
  IPAddress broker_address;
  void load();
  void save();
  bool valid()
  {
    return header[0] == 211 && header[1] == 51;
  }
  void init(EthernetClient &);
};
#ifdef USEMQTT
enum ParsingState { ps_unknown, ps_processing_config, ps_setting_output, ps_skipping };
enum Field { f_name, f_config, f_dig, f_pin, f_setting};
enum Setting { s_on, s_off, s_pwm, s_value, s_unknown, s_in, s_out };
#endif
enum InputStates { idle, reading, command_loaded };
bool mq_inet_aton(const char *ipstring, byte *addr);
#ifdef USEMQTT
void callback(char* topic, byte* payload, unsigned int length);
#endif
int readAnalogueValue(int pin);
int getNumber(char *buf_start, int &offset);
int getHexNumber(char *buf_start, int &offset);
float getFloat(char *buf_start, int &offset);
int getString(char *buf_start, int &offset);
bool opposite(float a, float b);
int freeRam ();
EthernetClient enet_client;
ProgramSettings program_settings;
unsigned long now;
unsigned long publish_time;
uint16_t port = 1883;
byte MAC_ADDRESS[] = { 0x00, 0x01, 0x03, 0x41, 0x30, 0xA5 }; // old 3com card
#ifdef USEMQTT
char config_topic[30];
char message_buf[100];
#endif
PubSubClient client(enet_client);
#ifdef USEMQTT
int pin_settings[72];
#endif
const int INPUT_BUFSIZE = 60;
const int START_MARK = '>';
const int END_MARK = '\n';
const char *RESPONSE_START = "<";
InputStates input_state = idle;
char command[INPUT_BUFSIZE];
int input_pos = 0;
char paramString[40];
void ProgramSettings::init(EthernetClient &enet_client)
{
  bool need_save = false;
  load();
  if (!program_settings.valid())
  {
    need_save = true;
    program_settings.header[0] = 211;
    program_settings.header[1] = 51;
    strcpy(program_settings.broker_host,"192.168.2.1");
    strcpy(program_settings.hostname,"Mega1");
    for (byte i = 0; i<6; i++)
      program_settings.mac_address[i] = MAC_ADDRESS[i];
    // setup the broker up address default (ethernet is not available yet)
    broker_ip[0] = 192;
    broker_ip[1] = 168;
    broker_ip[2] = 2;
    broker_ip[3] = 1;
  }
  Ethernet.begin(mac_address);
  DNSClient dns;
  dns.begin(dns_address);
  if (dns.getHostByName(broker_host, broker_address) == 1)
  {
    for (int i=0; i<4; ++i) broker_ip[i] = broker_address[i];
  }
  if (need_save)
    program_settings.save();
}
void ProgramSettings::load()
{
  int addr = 0;
  byte* p = (byte*)this;
  while (addr < sizeof(program_settings))
  {
    *p++ = EEPROM.read(addr++);
  }
}
bool mq_inet_aton(const char *ipstring, IPAddress &addr)
{
  return false;
}
void ProgramSettings::save()
{
  int addr = 0;
  byte* p = (byte*)this;
  while (addr < sizeof(program_settings))
  {
    EEPROM.write(addr++, *p++);
  }
}
#ifdef USEMQTT
void callback(char* topic, byte* payload, unsigned int length)
{
  unsigned int i = 0;
  ParsingState parse_state = ps_unknown;
  int pin = -1;
  Serial.print("Message arrived\n  topic: ");
  Serial.println(topic);
  Serial.print("Message length: ");
  Serial.println(length);
  payload[length] = 0; // TBD dangerous
  Field field = f_name;
  int j = 0;
  unsigned int n = strlen(topic);
  bool analogue_pin = false; // changes to true if the topic refers to an analogue pin
  for (i=0; i<=n; i++)
  {
    char curr = (i<n) ? topic[i] : 0;
    if (curr == '/' || curr == ' ' || i == n )
    {
      message_buf[j] = 0;
      if (field == f_name) field = f_config; // ignore
      else if (field == f_config)
      {
        if (strcmp(message_buf, "config") == 0)
        {
          parse_state = ps_processing_config;
          field = f_dig;
        }
        else if (strcmp(message_buf, "dig") == 0)
        {
          parse_state = ps_setting_output;
          field = f_pin; // already found f_dig
        }
        else if (strcmp(message_buf, "pwm") == 0)
        {
          parse_state = ps_setting_output;
          field = f_pin; // already found f_dig
          analogue_pin = true; // well, not strictly an analoge pin but may as well be
        }
        else
        {
          parse_state = ps_skipping;
          break;
        }
      }
      else if (field == f_dig)
      {
        if (strcmp(message_buf, "dig") == 0)
        {
          if (parse_state == ps_unknown) parse_state = ps_setting_output;
          field = f_pin; // found f_dig
        }
        else if (strcmp(message_buf, "ana") == 0 && parse_state == ps_processing_config)
        {
          analogue_pin = true;
          field = f_pin; // found analogue config
        }
        else
        {
          parse_state = ps_skipping;
          break;
        }
      }
      else if (field == f_pin)
      {
        int pos = 0;
        pin = getNumber(message_buf, pos);
        if (pos == 0)
        {
          parse_state = ps_skipping;
          break;
        }
        field = f_setting;
        Setting setting = s_unknown;
        if (strncmp((const char *)payload, "IN", length) == 0) setting = s_in;
        else if (strncmp((const char *)payload, "IGNORE", length) == 0) setting = s_unknown;
        else if (strncmp((const char *)payload, "AIN", length) == 0) setting = s_value;
        else if (strncmp((const char *)payload, "OUT", length) == 0) setting = s_out;
        else if (strncmp((const char *)payload, "PWM", length) == 0) setting = s_pwm;
        else if (strncmp((const char *)payload, "ON", length) == 0
                 || strncmp((const char *)payload, "on", length) == 0) setting = s_on;
        else if (strncmp((const char *)payload, "OFF", length) == 0
                 || strncmp((const char *)payload, "off", length) == 0) setting = s_off;
        else if (pin_settings[pin] != s_pwm)
        {
          Serial.println ("unknown setting type: ");
          ((char *)payload)[length] = 0;
          Serial.println((const char *)payload);
          break;
        }
        if (parse_state == ps_processing_config)
        {
          if (setting == s_out)
          {
            if (pin < 64)
            {
              pinMode(pin, OUTPUT);
              pin_settings[pin] = s_out;
              snprintf(message_buf, 99, "%s/dig/%d", program_settings.hostname, pin);
#ifdef FEEDBACK
              Serial.print("subscribing to: ");
              Serial.println(message_buf);
#endif
              client.subscribe(message_buf);
            }
          }
          else if (setting == s_in)
          {
            if (pin <64)
            {
              pinMode(pin, INPUT);
              pin_settings[pin] = s_in;
              snprintf(message_buf, 99, "%s/dig/%d", program_settings.hostname, pin);
              const char *status = (digitalRead(pin)) ? "on" : "off";
#ifdef FEEDBACK
              Serial.print("publishing to: ");
              Serial.println(message_buf);
#endif
              client.publish(message_buf, (uint8_t*)status, strlen(status), true );
            }
          }
          else if (setting == s_value)
          {
            if (pin < 8)
            {
              pin_settings[64 + pin] = s_value;
              snprintf(message_buf, 99, "%s/ana/%d", program_settings.hostname, pin);
              char status[10];
              int value = readAnalogueValue(pin);
              snprintf(status, 10, "%d", value);
#ifdef FEEDBACK
              Serial.print("publishing ");
              Serial.print(value);
              Serial.print(" to: ");
              Serial.println(message_buf);
#endif
              client.publish(message_buf, (uint8_t*)status, strlen(status), true );
            }
          }
          else if (setting == s_unknown)
          {
            if (analogue_pin)
            {
              if (pin < 8) pin_settings[64 + pin] = s_unknown;
              Serial.print("stopped publishing analogue pin ");
            }
            else if (pin < 64)
            {
              Serial.print("stopped publishing pin ");
              pin_settings[pin] = s_unknown;
            }
            Serial.println(pin);
          }
          else if (setting == s_pwm)
          {
            if (pin < 64)
            {
              pinMode(pin, OUTPUT);
              pin_settings[pin] = s_pwm;
              snprintf(message_buf, 99, "%s/pwm/%d", program_settings.hostname, pin);
#ifdef FEEDBACK
              Serial.print("subscribing to: ");
              Serial.println(message_buf);
#endif
              client.subscribe(message_buf);
            }
          }
        }
        else if (parse_state == ps_setting_output)
        {
          if (setting == s_on)
          {
            digitalWrite(pin, HIGH);
            Serial.print("turned pin ");
            Serial.print(pin);
            Serial.println(" on");
          }
          else if (setting == s_off)
          {
            digitalWrite(pin, LOW);
            Serial.print("turned pin ");
            Serial.print(pin);
            Serial.println(" off");
          }
          else if (pin < 14 && pin_settings[pin] == s_pwm)
          {
            int pos = 0;
            int val = getNumber((char *)payload, pos);
            if (pos)
            {
              Serial.print("setting pin ");
              Serial.print(pin);
              Serial.print(" to ");
              Serial.println(val);
              analogWrite(pin, val);
            }
          }
        }
        break;
      }
      j = 0;
    }
    else
    {
      message_buf[j++] = curr;
    }
  }
  if (parse_state == ps_skipping)
    Serial.println(" parse error");
}
#endif
int readAnalogueValue(int pin)
{
  int value = 0;
  if (pin == 0) value = analogRead(A0);
  else if (pin == 1) value = analogRead(A1);
  else if (pin == 2) value = analogRead(A2);
  else if (pin == 3) value = analogRead(A3);
  else if (pin == 4) value = analogRead(A4);
  else if (pin == 5) value = analogRead(A5);
  else if (pin == 6) value = analogRead(A6);
  else if (pin == 7) value = analogRead(A7);
  return value;
}
int getNumber(char *buf_start, int &offset)
{
  char *p = buf_start + offset;
  int res = 0;
  while (*p == ' ')
  {
    ++offset;
    p++;
  }
  int ch = *p;
  while (ch >= '0' && ch <= '9')
  {
    res = res * 10 + (ch - '0');
    ++offset;
    p++;
    ch = *p;
  }
  return res;
}
char upper(char ch)
{
  if (ch>='a' && ch<='z') ch = ch - 'a' + 'A';
  return ch;
}
int getHexNumber(char *buf_start, int &offset)
{
  char *p = buf_start + offset;
  int res = 0;
  while (*p == ' ')
  {
    ++offset;
    p++;
  }
  int ch = upper(*p);
  while ( (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <='F'))
  {
    res = res * 16;
    if (ch <= '9')
      res = res + (ch - '0');
    else
      res = res + (ch - 'A') + 10;
#ifdef DEBUG_CONSOLE
    Serial.print("hex: ");
    Serial.print(res);
    Serial.print(" ");
#endif
    ++offset;
    p++;
    ch = upper(*p);
  }
  return res;
}
float getFloat(char *buf_start, int &offset)
{
  bool seenDecimalPoint = false;
  char *p = buf_start + offset;
  float res = 0.0f;
  float frac = 1.0f;
  while (*p == ' ')
  {
    ++offset;
    p++;
  }
  int ch = *p;
  while ( (ch >= '0' && ch <= '9') || (ch == '.' && !seenDecimalPoint) )
  {
    if (ch == '.')
      seenDecimalPoint = true;
    else
    {
      int val = ch - '0';
      if (!seenDecimalPoint)
        res = res * 10.0 + (float)val;
      else
      {
        frac = frac/10.0f;
        res = res + frac * val;
      }
    }
    ++offset;
    p++;
    ch = *p;
  }
  return res;
}
int getString(char *buf_start, int &offset)
{
  char *p = buf_start + offset;
  while (*p == ' ')
  {
    ++offset;  // skip leading spaces
    p++;
  }
  char *q = paramString;
  while (q - paramString < 39 && *p && *p != ' ')
  {
    *q++ = *p++;
  }
  *q = 0;
  return q - paramString;
}
bool opposite(float a, float b)
{
  if (a<0 && b>0) return true;
  if (a>0 && b<0) return true;
  return false;
}
int freeRam ()
{
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}
void setup()
{
  Serial.begin(115200);
  Serial.print("Memory used: ");
  Serial.print(freeRam());
  now = millis();
  publish_time = now + 5000; // startup delay before we start publishing
#ifdef USEMQTT
  program_settings.init(enet_client);
  client.setCallback(callback);
  client.setServerIP(program_settings.broker_ip);
  client.setPort(program_settings.broker_port);
  if (Ethernet.begin(program_settings.mac_address) == 0)
  {
    Serial.println("Failed to configure Ethernet using DHCP");
    return;
  }
  //  client = PubSubClient(program_settings.hostname, program_settings.broker_port, callback, enet_client);
#endif
#ifdef USEMQTT
  for (int i=0; i<72; ++i) pin_settings[i] = s_unknown;
#endif
}
void loop()
{
#ifdef USEMQTT
  if (!client.connected())
  {
    // clientID, username, MD5 encoded password
    client.connect("mquino", "mquino_user", "00000000000000000000000000000");
    if (!client.connected()) Serial.println("connection failed");
    else
    {
      snprintf(config_topic, 29, "%s/config/dig/+", program_settings.hostname);
      client.subscribe(config_topic);
      snprintf(config_topic, 29, "%s/config/ana/+", program_settings.hostname);
      client.subscribe(config_topic);
    }
  }
#endif
  if (client.connected()) client.loop();
  now = millis();
  bool response_required = false;
  const char *error_message = 0;
  int chars_ready = Serial.available();
  if (input_state != command_loaded && chars_ready)
  {
    int ch = Serial.read();
#ifdef DEBUG_CONSOLE
    Serial.println(ch);
#endif
    switch (input_state)
    {
    case idle:
      if (ch == START_MARK)
      {
        input_state = reading;
#ifdef DEBUG_CONSOLE
        Serial.print("reading (");
        Serial.print(chars_ready);
        Serial.println(")");
#endif
      }
      break;
    case reading:
      if (ch == END_MARK)
      {
#ifdef DEBUG_CONSOLE
        Serial.println("end mark");
#endif
        if (input_pos == 0)
        {
          input_state = idle; // no command read
#ifdef DEBUG_CONSOLE
          Serial.println("idle");
#endif
        }
        else
        {
          input_state = command_loaded;
#ifdef DEBUG_CONSOLE
          Serial.println("loaded");
#endif
        }
#ifdef DEBUG_CONSOLE
        Serial.print("buf: ");
        Serial.println(command);
#endif
        break;
      }
      command[input_pos++] = ch;
      if (input_pos >= INPUT_BUFSIZE) // buffer overrun
      {
        input_state = idle;
        input_pos = 0;
      }
      command[input_pos] = 0; // keep the input string terminated
      break;
    case command_loaded:
      break;
    default:
      ;
    }
  }
  else if (input_state == command_loaded)
  {
#ifdef DEBUG_CONSOLE
    Serial.println("command loaded");
#endif
    response_required = true;
    char cmd = command[0];
    int scan = 1;
    int param1 = getNumber(command, scan); // read number from this index
    int param2 = getNumber(command, scan); // read the paramer
    int paramLen = getString(command, scan);
    if (cmd == '?')
    {
      Serial.print(RESPONSE_START);
      Serial.println("mquino v0.2 Jan 28, 2013");
    }
    else if (cmd == 'd')
    {
      Serial.print("host      : ");
      Serial.println(program_settings.hostname);
      Serial.print("default ip: ");
      for (byte i=0; i<4; ++i)
      {
        Serial.print(program_settings.ip[i], DEC);
        if (i<3) Serial.print('.');
      }
      Serial.println();
      Serial.print("broker    : ");
      Serial.println(program_settings.broker_host);
      Serial.print("port      : ");
      Serial.println(program_settings.broker_port);
      Serial.print("mac       : ");
      for (byte i=0; i<6; ++i)
      {
        if (program_settings.mac_address[i] < 10)
          Serial.print('0');
        Serial.print(program_settings.mac_address[i], HEX);
        if (i<5) Serial.print(':');
      }
      Serial.println();
#ifdef USEMQTT
      Serial.print("current ip: ");
      for (byte i = 0; i < 4; i++)
      {
        Serial.print(Ethernet.localIP()[i], DEC);
        if (i<3) Serial.print(".");
      }
#endif
      Serial.println();
    }
    else if (cmd == 'h')
    {
      scan = 1;
      paramLen = getString(command, scan);
      if (paramLen < 40)
      {
        strcpy(program_settings.hostname, paramString);
        Serial.print("hostname set to ");
        Serial.println(paramString);
      }
    }
    else if (cmd == 'b')
    {
      scan = 1;
      paramLen = getString(command, scan);
      if (paramLen < 40)
      {
        strcpy(program_settings.broker_host, paramString);
        Serial.print("broker host set to ");
        Serial.println(paramString);
        DNSClient dns;
        dns.begin(program_settings.dns_address);
        if (!mq_inet_aton(paramString, program_settings.broker_address))
          if (dns.getHostByName(paramString, program_settings.broker_address) != 1)
            Serial.println("failed to translate broker host to an address");
      }
    }
    else if (cmd == 'p')
    {
      program_settings.broker_port = param1;
      Serial.print("port set to ");
      Serial.println(param1);
    }
    else if (cmd == 's')
    {
      scan = 1;
      program_settings.save();
    }
    else if (cmd == 'm')
    {
      scan = 1;
      int i = 0;
      while (i<6 && command[scan] != 0)
      {
        program_settings.mac_address[i] = getHexNumber(command, scan);
        if (command[scan] == 0) break;
        ++scan;
        ++i;
      }
      Serial.print("MAC address is now: ");
      for (int i=0; i<6; ++i)
      {
        if (program_settings.mac_address[i] < 10)
          Serial.print('0');
        Serial.print(program_settings.mac_address[i], HEX);
        if (i<5) Serial.print(':');
      }
      Serial.println();
    }
    else if (cmd == 'i')
    {
      scan = 1;
      int i = 0;
      while (i<4 && command[scan] != 0)
      {
        program_settings.ip[i] = getNumber(command, scan);
        if (command[scan] == 0) break;
        ++scan;
        ++i;
      }
      Serial.print("IP address is now: ");
      for (int i=0; i<4; ++i)
      {
        Serial.print(program_settings.ip[i], DEC);
        if (i<3) Serial.print('.');
      }
      Serial.println();
    }
    else if (cmd == 'F')
    {
      if (param1 >= 0 && param1 <= 7)
      {
        if (param1 == 0) param1 = A0;
        else if (param1 == 1) param1 = A1;
        else if (param1 == 2) param1 = A2;
        else if (param1 == 3) param1 = A3;
        else if (param1 == 4) param1 = A4;
        else if (param1 == 5) param1 = A5;
        else if (param1 == 6) param1 = A6;
        else if (param1 == 7) param1 = A7;
        else param1 = -1;
        if (param1 >= 0)
        {
          Serial.print(RESPONSE_START);
          Serial.println( analogRead( param1 ) );
        }
      }
      else
        error_message = "Analogue reads are only available for ports 0..7";
    }
    else if (cmd == 'I')
    {
      if (param1 >= 0 && param1 <= 64)
      {
        Serial.print(RESPONSE_START);
        if (digitalRead(param1))
          Serial.println("H");
        else
          Serial.println("L");
      }
      else
        error_message = "invalid port";
    }
    else if (cmd == 'O')
    {
      if (param1 >= 0 && param1 <= 64 && paramLen == 1)
        if (paramString[0] == 'H')
          digitalWrite(param1, HIGH);
        else if (paramString[0] == 'L')
          digitalWrite(param1, LOW);
        else
          error_message = "bad output state";
      else
        error_message = "invalid port";
    }
done_command:
    Serial.println(command);
    // remove the command from the input buffer
    char *p = command;
    char *q = command + input_pos;
    while (*q)
    {
      *p++ = *q++;
    }
    *p = 0;
    input_pos = p - command;
    input_state = idle;
    if (error_message)
    {
      Serial.print(RESPONSE_START);
      Serial.println(error_message);
    }
    else if (response_required)
    {
      Serial.print(RESPONSE_START);
      Serial.println("OK");
    }
  }
#ifdef USEMQTT
  if (publish_time <= now)
  {
    for (byte i = 0; i<64; ++i)
    {
      if (pin_settings[i] == s_in)
      {
        snprintf(message_buf, 99, "%s/dig/%d", program_settings.hostname, i);
        const char *status = (digitalRead(i)) ? "on" : "off";
        client.publish(message_buf, (uint8_t*)status, strlen(status), true );
        Serial.print(message_buf);
        Serial.print("\t");
        Serial.println(status);
      }
    }
    for (byte i = 0; i<8; ++i)
    {
      if (pin_settings[64 + i] == s_value)
      {
        snprintf(message_buf, 99, "%s/ana/%d", program_settings.hostname, i);
        char status[10];
        int value = readAnalogueValue(i);
        snprintf(status, 10, "%d", value);
        client.publish(message_buf, (uint8_t*)status, strlen(status), true );
        Serial.print(message_buf);
        Serial.print("\t");
        Serial.println(status);
      }
    }
    publish_time += 200;
  }
#endif
}

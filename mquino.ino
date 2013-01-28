#include <Arduino.h>
#include <EEPROM.h>
#define DEBUG 1
#define USEMQTT 1
#ifdef USEMQTT
#include <SPI.h>
#include <PubSubClient.h>
#include <Ethernet.h>
#endif
struct ProgramSettings
{
  byte header[2];
  char hostname[40];
  byte ip[4];
  byte mac_address[6];
  char broker_host[40];
  int broker_port;
  void load();
  void save();
  bool valid()
  {
    return header[0] == 217 && header[1] == 59;
  }
};
#ifdef USEMQTT
enum ParsingState { ps_unknown, ps_processing_config, ps_setting_output, ps_skipping };
enum Field { f_name, f_config, f_dig, f_pin, f_setting};
enum Setting { s_on, s_off, s_pwm, s_value, s_unknown, s_in, s_out };
#endif
enum InputStates { idle, reading, command_loaded };
#ifdef TESTING
class Test
{
public:
  void run();
  virtual bool execute() = 0;
  inline static std::list<Test *>::iterator begin()
  {
    return all_tests.begin();
  }
  inline static std::list<Test *>::iterator end()
  {
    return all_tests.end();
  }
  static void add(Test *test)
  {
    all_tests.push_back(test);
  }
  static int total()
  {
    return total_tests;
  }
  static int failures()
  {
    return total_failures;
  }
  static int successes()
  {
    return total_successes;
  }
protected:
  static int total_tests;
  static int total_failures;
  static int total_successes;
private:
  static std::list<Test *> all_tests;
};
#endif
#ifdef USEMQTT
void callback(char* topic, byte* payload, unsigned int length);
#endif
int getNumber(char *buf_start, int &offset);
int getHexNumber(char *buf_start, int &offset);
float getFloat(char *buf_start, int &offset);
int getString(char *buf_start, int &offset);
bool opposite(float a, float b);
ProgramSettings program_settings;
unsigned long now;
unsigned long publish_time;
#ifdef USEMQTT
char MQTT_SERVER[40];
char HOSTNAME[40];
char config_topic[30];
uint16_t port = 1883;
byte MAC_ADDRESS[] = { 0x00, 0x01, 0x03, 0x41, 0x30, 0xA5 }; // old 3com card
byte server[] = { 192, 168, 4, 28 };
char message_buf[100];
EthernetClient enet_client;
PubSubClient client(server, 1883, callback, enet_client);
#endif
#ifdef USEMQTT
int pin_settings[64];
#endif
const int INPUT_BUFSIZE = 60;
const int START_MARK = '>';
const int END_MARK = '\n';
const char *RESPONSE_START = "<";
InputStates input_state = idle;
char command[INPUT_BUFSIZE];
int input_pos = 0;
char paramString[40];
void ProgramSettings::load()
{
  int addr = 0;
  byte* p = (byte*)this;
  while (addr < sizeof(program_settings))
  {
    *p++ = EEPROM.read(addr++);
  }
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
#ifdef TESTING
class TestSettingsSave : public Test
{
  int testNum;
public:
  TestSettingsSave(int test) : testNum(test) {  }
  bool execute()
  {
    if (testNum == 1) return testOne();
  }
  bool testOne()
  {
    program_settings.header[0] = 217;
    program_settings.header[1] = 59;
    strcpy(program_settings.hostname, "TestOneHost");
    program_settings.broker_port = 5594;
    program_settings.save();
    program_settings.broker_port = 2225;
    strcpy(program_settings.hostname, "EMPTY");
    program_settings.load();
    if (program_settings.broker_port != 5594
        || strcmp(program_settings.hostname, "TestOneHost") != 0)
      return false;
    else
      return true;
  }
};
#endif
#ifdef TESTING
int Test::total_tests = 0;
int Test::total_failures = 0;
int Test::total_successes = 0;
std::list<Test *> Test::all_tests;
#endif
#ifdef TESTING
void Test::run()
{
  ++total_tests;
  if (this->execute())
    ++total_successes;
  else
    ++total_failures;
}
#endif
#ifdef USEMQTT
void callback(char* topic, byte* payload, unsigned int length)
{
  unsigned int i = 0;
  ParsingState parse_state = ps_unknown;
  int pin = -1;
  Serial.println("Message arrived:  topic: " + String(topic));
  Serial.println("Length: " + String(length,DEC));
  // create character buffer with ending null terminator (string)
  Field field = f_name;
  int j = 0;
  for (i=0; i<length; i++)
  {
    char curr = payload[i];
    if (curr == '/' || curr == ' ' || i + 1 == length)
    {
      message_buf[j] = 0;
      if (field == f_name) field = f_config; // ignore
      else if (field == f_config)
      {
        if (strcmp(message_buf, "config") == 0)
        {
          parse_state = ps_processing_config;
          field = f_pin;
        }
        else if (strcmp(message_buf, "dig") == 0)
        {
          parse_state = ps_setting_output;
          field = f_pin; // already found f_dig
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
          parse_state = ps_setting_output;
          field = f_pin; // found f_dig
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
      }
      else if (field == f_setting)
      {
        Setting setting = s_unknown;
        if (strcmp(message_buf, "IN")) setting = s_in;
        else if (strcmp(message_buf, "OUT")) setting = s_out;
        else if (strcmp(message_buf, "PWM")) setting = s_pwm;
        else if (strcmp(message_buf, "on")) setting = s_on;
        else if (strcmp(message_buf, "off")) setting = s_off;
        else
        {
          Serial.println ("unknown setting type");
          break;
        }
        if (parse_state == ps_processing_config)
        {
          if (setting == s_out)
          {
            pinMode(pin, OUTPUT);
            snprintf(message_buf, 99, "%s/pin/%d", program_settings.hostname, pin);
            client.subscribe(message_buf);
            if (pin < 64) pin_settings[pin] = s_in;
          }
          else if (setting == s_in)
          {
            pinMode(pin, INPUT);
            snprintf(message_buf, 99, "%s/pin/%d", program_settings.hostname, pin);
            const char *status = (digitalRead(pin)) ? "on" : "off";
            client.publish(message_buf, (uint8_t*)status, strlen(status), true );
            if (pin <64) pin_settings[pin] = s_in;
          }
          else if (setting == s_pwm)
          {
            Serial.println ("PWM mode is not currently supported");
          }
        }
        else if (parse_state == ps_setting_output)
        {
          if (setting == s_on)
            digitalWrite(pin, HIGH);
          else if (setting == s_off)
            digitalWrite(pin, LOW);
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
#ifdef DEBUG
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
#ifdef TESTING
class TestGetFloat : public Test
{
  int testNum;
public:
  TestGetFloat(short test) : testNum(test) {  }
  bool execute()
  {
    if (testNum == 1) return testOne();
  }
  bool testOne()
  {
    strcpy(command, "z 123.546 X");
    int offset = 1;
    float val = getFloat(command, offset);
    if (val == 123.546f)
      return true;
    else
    {
      std::cout << "Error, expected " << 123.546 << " got " << val << "\n";
      return false;
    }
  }
};
#endif
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
void setup()
{
  Serial.begin(115200);
  program_settings.load();
  now = millis();
  publish_time = now + 5000; // startup delay before we start publishing
#ifdef USEMQTT
  strcpy(MQTT_SERVER,"192.168.4.28");
  strcpy(HOSTNAME,"MyMega");
  if (Ethernet.begin(MAC_ADDRESS) == 0)
  {
    Serial.println("Failed to configure Ethernet using DHCP");
    return;
  }
  client = PubSubClient((char *)MQTT_SERVER, port, callback, enet_client);
#endif
#ifdef USEMQTT
  for (int i=0; i<64; ++i) pin_settings[i] = s_unknown;
#endif
}
void loop()
{
#ifdef USEMQTT
  if (!client.connected())
  {
    // clientID, username, MD5 encoded password
    client.connect("mquino", "mquino_user", "00000000000000000000000000000");
    snprintf(config_topic, 29, "%s/config/+", HOSTNAME);
    client.subscribe(config_topic);
  }
#endif
  now = millis();
  bool response_required = false;
  const char *error_message = 0;
  int chars_ready = Serial.available();
  if (input_state != command_loaded && chars_ready)
  {
    int ch = Serial.read();
#ifdef DEBUG
    Serial.println(ch);
#endif
    switch (input_state)
    {
    case idle:
      if (ch == START_MARK)
      {
        input_state = reading;
#ifdef DEBUG
        Serial.print("reading (");
        Serial.print(chars_ready);
        Serial.println(")");
#endif
      }
      break;
    case reading:
      if (ch == END_MARK)
      {
#ifdef DEBUG
        Serial.println("end mark");
#endif
        if (input_pos == 0)
        {
          input_state = idle; // no command read
#ifdef DEBUG
          Serial.println("idle");
#endif
        }
        else
        {
          input_state = command_loaded;
#ifdef DEBUG
          Serial.println("loaded");
#endif
        }
#ifdef DEBUG
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
#ifdef DEBUG
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
done_command:
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
  if (publish_time >= now)
  {
    for (byte i = 0; i<64; ++i)
    {
      if (pin_settings[i] == s_in)
      {
        snprintf(message_buf, 99, "%s/pin/%d", program_settings.hostname, i);
        const char *status = (digitalRead(i)) ? "on" : "off";
        client.publish(message_buf, (uint8_t*)status, strlen(status), true );
      }
    }
    publish_time += 1000;
  }
#endif
}

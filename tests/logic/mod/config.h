#pragma once
#include <map>
#include <string>
#include <cstdlib>
struct ConfigEntry { std::string def, cur;
  bool GetBool(){return atoi(cur.c_str())!=0;} int GetInt(){return atoi(cur.c_str());} float GetFloat(){return (float)atof(cur.c_str());} const char* GetString(){return cur.c_str();}
  void SetBool(bool v){cur=v?"1":"0";} void SetInt(int v){cur=std::to_string(v);} void SetFloat(float v){cur=std::to_string(v);} void SetString(const char*s){cur=s;} void Reset(){cur=def;} };
struct Config { int saves=0;
  ConfigEntry* Bind(const char*, const char* d, const char*){auto*e=new ConfigEntry;e->def=e->cur=d;return e;}
  ConfigEntry* Bind(const char*, int d, const char*){auto*e=new ConfigEntry;e->def=e->cur=std::to_string(d);return e;}
  ConfigEntry* Bind(const char*, float d, const char*){auto*e=new ConfigEntry;e->def=e->cur=std::to_string(d);return e;}
  ConfigEntry* Bind(const char*, bool d, const char*){auto*e=new ConfigEntry;e->def=e->cur=d?"1":"0";return e;}
  void Save(){++saves;} };
extern Config* cfg;

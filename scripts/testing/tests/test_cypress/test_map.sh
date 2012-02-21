#!/bin/bash
#% NUM_MASTERS = 3
#% NUM_HOLDERS = 0
#% SETUP_TIMEOUT = 3

echo '{do=set; path = "/map"; value={hello=world; list=[0;a;{}]; n=1}}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=set; path="/map/hello"; value=not_world}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=set; path="/map/list/2/some"; value=value}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=remove; path="/map/n"}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=set; path="/map/list"; value=[]}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=set; path="/map/list/+/a"; value=1}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=set; path="/map/list/-/b"; value=2}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=remove; path="/map/hello"}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

echo '{do=remove; path="/map"}' |ytdriver
echo '{do=get; path = "/map"}' |ytdriver

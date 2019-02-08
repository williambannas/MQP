import urllib.request
import urllib.parse
import json
import requests

API = 'http://192.168.2.3:1337'

with urllib.request.urlopen(API) as response:
        print(response.read())

test = {
        "pId": 1,
        "message": "Will is awesome"
        }

print(json.dumps(test, indent=4))

r = requests.post(API+"/api/pluto", json=test)
print(r.status_code, r.reason)
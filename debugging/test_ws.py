import asyncio
import websockets
import ssl
import json

URI = "wss://pilot3.local/ws/log?token=cece6d70e5f79d4ac3d74c3e0cfb86bb"

async def test_ws():
    # Ez a három sor garantálja, hogy a kliens vakon megbízik az ESP32-ben
    ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    print(f"Kapcsolódás: {URI} ...")
    try:
        async with websockets.connect(URI, ssl=ssl_context) as websocket:
            print("Sikeres csatlakozás! Várakozás az élő logokra...\n" + "-"*50)
            while True:
                raw_msg = await websocket.recv()
                try:
                    msg_obj = json.loads(raw_msg)
                    if msg_obj.get("type") == "log":
                        # Csak a tiszta log üzenetet írjuk ki (sortörés nélkül, mert az már benne van)
                        print(msg_obj.get("msg"), end="")
                except json.JSONDecodeError:
                    print(raw_msg)
    except Exception as e:
        print(f"Kapcsolódási hiba: {e}")

if __name__ == "__main__":
    asyncio.run(test_ws())


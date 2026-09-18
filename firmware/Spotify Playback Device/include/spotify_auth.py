import secrets
import hashlib
import base64
import urllib.parse

CLIENT_ID = "4825f6d62f3147ddbeb8b11a87039776"
REDIRECT_URI = "http://127.0.0.1:8888/callback"

SCOPES = [
    "user-read-playback-state",
    "user-read-currently-playing",
    "user-modify-playback-state",
    "user-library-read",
    "user-library-modify"
]

# Generate PKCE verifier
verifier = secrets.token_urlsafe(64)

# Generate SHA256 challenge
digest = hashlib.sha256(verifier.encode()).digest()

challenge = (
    base64.urlsafe_b64encode(digest)
    .decode()
    .rstrip("=")
)

params = {
    "client_id": CLIENT_ID,
    "response_type": "code",
    "redirect_uri": REDIRECT_URI,
    "scope": " ".join(SCOPES),
    "code_challenge_method": "S256",
    "code_challenge": challenge,
}

url = (
    "https://accounts.spotify.com/authorize?"
    + urllib.parse.urlencode(params)
)

print("\nCODE VERIFIER:\n")
print(verifier)

print("\nOPEN THIS URL:\n")
print(url)

import urllib.request
import json

print("\nAfter authorizing Spotify:")
redirect_url = input("Paste the entire redirect URL here: ").strip()

parsed = urllib.parse.urlparse(redirect_url)
params = urllib.parse.parse_qs(parsed.query)

if "code" not in params:
    print("ERROR: No authorization code found in URL.")
    exit()

code = params["code"][0]

print("\nAuthorization code extracted successfully.")

token_data = urllib.parse.urlencode({
    "client_id": CLIENT_ID,
    "grant_type": "authorization_code",
    "code": code,
    "redirect_uri": REDIRECT_URI,
    "code_verifier": verifier
}).encode()

request = urllib.request.Request(
    "https://accounts.spotify.com/api/token",
    data=token_data,
    headers={
        "Content-Type": "application/x-www-form-urlencoded"
    },
    method="POST"
)

with urllib.request.urlopen(request) as response:
    result = json.loads(response.read())

print("\nACCESS TOKEN:\n")
print(result["access_token"])

print("\nREFRESH TOKEN:\n")
print(result["refresh_token"])

print("\nExpires in:", result["expires_in"], "seconds")
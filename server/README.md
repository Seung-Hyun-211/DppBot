# Go-Local 서버

`DppBot` C++ 클라이언트가 로컬 HTTP로 호출하는 서버. 검색어/URL/플레이리스트를 받아 YouTube Data API로 메타데이터를 조회하고, `yt-dlp`로 다운로드해 `ffmpeg`로 PCM 변환한 뒤 MySQL에 기록한다.

전체 아키텍처와 설계 원칙은 [../DESIGN.md](../DESIGN.md)를 참고. 이 문서는 서버를 실제로 빌드/실행하고 API를 호출하는 법만 다룬다.

## 요구사항

- Go 1.24+
- MySQL (또는 MariaDB)
- YouTube Data API v3 키
- `yt-dlp`, `ffmpeg` — 실행 시 `PATH`에 있어야 함

## 설정

`config.example.json`을 복사해 실행 파일과 같은 디렉토리에 `config.json`으로 둔다. **`config.json`은 `.gitignore`에 포함되어 있으니 절대 커밋하지 않는다.**

```bash
cp config.example.json config.json
# config.json을 열어 실제 값으로 채운다
```

| 필드 | 설명 |
|---|---|
| `ApiKey` | YouTube Data API v3 키 |
| `ApplicationName` | 임의의 앱 이름 |
| `Database.Host` / `Port` / `User` / `Password` / `Database` | MySQL 접속 정보 |
| `Port` | HTTP 서버 포트. 비워두면 `:8080`. `:8080`처럼 콜론만 있으면 `127.0.0.1:8080`로 바인딩(로컬 전용). `host:port` 전체를 적으면 그대로 사용 |

테이블(`youtube_videos`)은 서버 시작 시 없으면 자동으로 생성된다.

## 빌드 / 실행

```bash
go build -o Go-Local .
./Go-Local
```

또는 `./build.sh` (Linux), `build-windows.bat` / `build-linux.bat`(Windows에서 각 플랫폼용 빌드).

실행하면 다음이 출력된다:

```
Server starting on http://127.0.0.1:8080
```

## API 사용법

모든 엔드포인트는 `GET`이며 `Content-Type: application/json`으로 응답한다.

### `GET /process`

검색어, YouTube URL, 또는 플레이리스트 URL을 받아 처리한다. `q`와 `url` 중 하나만 있으면 된다(`url`이 우선).

| 파라미터 | 설명 |
|---|---|
| `q` | 검색어 또는 YouTube URL |
| `url` | YouTube URL (검색 없이 바로 처리) |

**검색어로 호출**

```bash
curl -s "http://127.0.0.1:8080/process?q=이찬혁+장례희망"
```

```json
{
  "success": true,
  "metadata": {
    "id": "iIn_1_XDuBM",
    "title": "이찬혁 (LEE CHANHYUK) - '장례희망' LIVE CLIP",
    "channelId": "UCIcXK1CI8URMIqNO_1cehiQ",
    "channelTitle": "AKMU",
    "thumbnail": "https://i.ytimg.com/vi/iIn_1_XDuBM/mqdefault.jpg",
    "duration": 253
  },
  "local_path": "db/AKMU/이찬혁 (LEE CHANHYUK) - '장례희망' LIVE CLIP.pcm",
  "db_status": "Success"
}
```

**영상 URL로 호출**

```bash
curl -s "http://127.0.0.1:8080/process?url=https://www.youtube.com/watch?v=iIn_1_XDuBM"
```

**플레이리스트 URL로 호출** (`list=` 파라미터가 있으면 자동으로 플레이리스트로 처리됨)

```bash
curl -s "http://127.0.0.1:8080/process?url=https://www.youtube.com/playlist?list=PLxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

```json
{
  "success": true,
  "list": [
    { "id": "...", "title": "...", "channelId": "...", "channelTitle": "...", "thumbnail": "...", "localPath": "...", "duration": 253 }
  ],
  "total": 10,
  "success_count": 9,
  "failed_count": 1
}
```

**실패 응답**

```json
{ "success": false, "error": "No results found" }
```

### `GET /getrandom`

DB에 활성화(`activate=TRUE`)된 곡 중 무작위로 N개를 가져온다.

| 파라미터 | 설명 |
|---|---|
| `count` | 가져올 개수. 기본 10, 최대 100 |

```bash
curl -s "http://127.0.0.1:8080/getrandom?count=5"
```

```json
{
  "success": true,
  "count": 5,
  "items": [
    { "id": "...", "title": "...", "channelId": "", "channelTitle": "...", "thumbnail": "...", "localPath": "...", "duration": 253 }
  ]
}
```

### `GET /deactivate`

특정 곡을 추천 대상(`/getrandom`)에서 제외한다. 파일이나 DB row를 지우지 않고 `activate`만 `FALSE`로 바꾼다.

| 파라미터 | 설명 |
|---|---|
| `id` | YouTube videoId |

```bash
curl -s "http://127.0.0.1:8080/deactivate?id=iIn_1_XDuBM"
```

```json
{ "success": true, "message": "Video deactivated successfully", "id": "iIn_1_XDuBM" }
```

## 동작 참고

- 같은 영상에 대한 동시 요청은 파일 경로 단위 락으로 직렬화되어 중복 다운로드되지 않는다.
- 이미 `db/<채널명>/<제목>.pcm` 파일이 있으면 다운로드를 건너뛰고 바로 그 경로를 응답한다.
- `yt-dlp`는 403 에러 회피를 위해 `android` → `ios` → `web` player client를 순서대로 시도한다.

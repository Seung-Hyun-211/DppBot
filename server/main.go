package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"html"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	_ "github.com/go-sql-driver/mysql"
	"google.golang.org/api/option"
	"google.golang.org/api/youtube/v3"
)

// ================= 설정 =================

// DatabaseConfig 데이터베이스 설정 구조체
type DatabaseConfig struct {
	Host     string `json:"Host"`
	Port     int    `json:"Port"`
	User     string `json:"User"`
	Password string `json:"Password"`
	Database string `json:"Database"`
}

// Config 구조체
type Config struct {
	ApiKey          string         `json:"ApiKey"`
	ApplicationName string         `json:"ApplicationName"`
	Database        DatabaseConfig `json:"Database"`
	Port            string         `json:"Port,omitempty"` // 서버 포트 (기본값: ":8080")
}

var (
	appConfig      Config
	youtubeService *youtube.Service
	db             *sql.DB // 전역 커넥션 풀 (요청마다 새로 열지 않음)
)

// ================= 동시 다운로드 락 =================

// keyedMutex 는 같은 key(여기서는 대상 파일 경로)에 대한 동시 접근을 직렬화한다.
// 같은 영상을 동시에 두 번 다운로드하는 것을 막기 위해 사용한다.
type keyedMutex struct {
	mu    sync.Mutex
	locks map[string]*sync.Mutex
}

func newKeyedMutex() *keyedMutex {
	return &keyedMutex{locks: make(map[string]*sync.Mutex)}
}

func (m *keyedMutex) Lock(key string) (unlock func()) {
	m.mu.Lock()
	l, ok := m.locks[key]
	if !ok {
		l = &sync.Mutex{}
		m.locks[key] = l
	}
	m.mu.Unlock()

	l.Lock()
	return l.Unlock
}

var downloadLocks = newKeyedMutex()

// ================= 유틸 =================

func getExeDir() string {
	exe, err := os.Executable()
	if err != nil {
		cwd, _ := os.Getwd()
		return cwd
	}

	dir := filepath.Dir(exe)
	if strings.Contains(dir, "go-build") || strings.Contains(dir, "Temp") || strings.Contains(dir, "tmp") {
		cwd, _ := os.Getwd()
		return cwd
	}
	return dir
}

func sanitizeFilename(name string) string {
	reg := regexp.MustCompile(`[\\/:*?"<>|]`)
	sanitized := reg.ReplaceAllString(name, " ")
	return strings.TrimSpace(regexp.MustCompile(`\s+`).ReplaceAllString(sanitized, " "))
}

// parseISO8601Duration ISO 8601 duration 형식을 초 단위로 변환 (예: PT4M13S -> 253)
func parseISO8601Duration(duration string) int {
	reg := regexp.MustCompile(`PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?`)
	matches := reg.FindStringSubmatch(duration)
	if len(matches) < 4 {
		return 0
	}

	hours := 0
	minutes := 0
	seconds := 0

	if matches[1] != "" {
		hours, _ = strconv.Atoi(matches[1])
	}
	if matches[2] != "" {
		minutes, _ = strconv.Atoi(matches[2])
	}
	if matches[3] != "" {
		seconds, _ = strconv.Atoi(matches[3])
	}

	return hours*3600 + minutes*60 + seconds
}

// splitAtFirstAny s를 seps 중 가장 먼저 나오는 구분자 앞까지 잘라 반환한다.
func splitAtFirstAny(s string, seps ...string) string {
	end := len(s)
	for _, sep := range seps {
		if i := strings.Index(s, sep); i != -1 && i < end {
			end = i
		}
	}
	return s[:end]
}

// extractVideoID 입력 문자열(URL 또는 ID)에서 videoID를 추출한다.
// youtube.com/youtu.be의 서브도메인(www, m, music 등)에 관계없이 동작한다.
// v=, youtu.be/ 패턴이 없으면 빈 문자열을 반환한다.
func extractVideoID(input string) string {
	if idx := strings.Index(input, "youtu.be/"); idx != -1 {
		rest := input[idx+len("youtu.be/"):]
		return splitAtFirstAny(rest, "?", "&", "/")
	}
	if idx := strings.Index(input, "v="); idx != -1 {
		rest := input[idx+len("v="):]
		return splitAtFirstAny(rest, "&", "?")
	}
	return ""
}

// extractPlaylistID 입력 문자열에서 플레이리스트 ID(list=...)를 추출한다.
func extractPlaylistID(input string) string {
	reg := regexp.MustCompile(`(?:list=)([a-zA-Z0-9_-]+)`)
	matches := reg.FindStringSubmatch(input)
	if len(matches) <= 1 {
		return ""
	}

	playlistID := matches[1]
	if decoded, err := url.QueryUnescape(playlistID); err == nil {
		playlistID = decoded
	}
	return strings.TrimSpace(playlistID)
}

func hasStartRadio(input string) bool {
	return strings.Contains(input, "start_radio=")
}

func isYoutubeURL(input string) bool {
	return strings.Contains(input, "youtube.com") || strings.Contains(input, "youtu.be")
}

func getTargetPCMPath(channelTitle, title string) string {
	return filepath.Join(getExeDir(), "db", sanitizeFilename(channelTitle), sanitizeFilename(title)+".pcm")
}

// ================= 데이터 모델 =================

type VideoResult struct {
	ID    string `json:"id"`
	Title string `json:"title"`
}

type JSONResponse struct {
	Success       bool                   `json:"success"`
	Error         string                 `json:"error,omitempty"`
	Metadata      map[string]interface{} `json:"metadata,omitempty"`
	SearchResults []VideoResult          `json:"search_results,omitempty"`
	LocalPath     string                 `json:"local_path,omitempty"`
	DBStatus      string                 `json:"db_status,omitempty"`
}

// PlayItem 재생용 아이템 구조체 (플레이리스트 / getrandom 응답에서 공통으로 사용)
type PlayItem struct {
	ID           string `json:"id"`
	Title        string `json:"title"`
	ChannelID    string `json:"channelId"`
	ChannelTitle string `json:"channelTitle"`
	Thumbnail    string `json:"thumbnail"`
	LocalPath    string `json:"localPath"`
	Duration     int    `json:"duration"`
}

// ================= 설정 / 외부 서비스 초기화 =================

func loadConfig() error {
	configPath := filepath.Join(getExeDir(), "config.json")
	file, err := os.ReadFile(configPath)
	if err != nil {
		return err
	}
	return json.Unmarshal(file, &appConfig)
}

func initYouTubeService() error {
	ctx := context.Background()
	var err error
	youtubeService, err = youtube.NewService(ctx, option.WithAPIKey(appConfig.ApiKey))
	return err
}

func buildDSN() string {
	cfg := appConfig.Database
	if cfg.Port == 0 {
		cfg.Port = 3306
	}
	return fmt.Sprintf("%s:%s@tcp(%s:%d)/%s?charset=utf8mb4&parseTime=true",
		cfg.User, cfg.Password, cfg.Host, cfg.Port, cfg.Database)
}

// ================= DB =================

func ensureTableExists() error {
	dbName := appConfig.Database.Database

	query := `SELECT COUNT(*) FROM information_schema.tables
	          WHERE table_schema = ? AND table_name = 'youtube_videos'`
	var count int
	if err := db.QueryRow(query, dbName).Scan(&count); err != nil {
		return err
	}

	if count == 0 {
		createTableQuery := `CREATE TABLE youtube_videos (
			id VARCHAR(255) PRIMARY KEY,
			title VARCHAR(500) NOT NULL,
			channel_id VARCHAR(255) NOT NULL,
			duration INT DEFAULT 0,
			thumbnail VARCHAR(500),
			kind VARCHAR(100),
			etag VARCHAR(255),
			id_kind VARCHAR(100),
			id_video_id VARCHAR(255),
			id_channel_id VARCHAR(255),
			id_playlist_id VARCHAR(255),
			snippet_published_at DATETIME,
			snippet_description TEXT,
			snippet_channel_title VARCHAR(255),
			snippet_live_broadcast_content VARCHAR(50),
			snippet_thumbnails JSON,
			raw_search_data JSON,
			activate BOOLEAN DEFAULT TRUE,
			created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci`

		if _, err := db.Exec(createTableQuery); err != nil {
			return fmt.Errorf("failed to create table: %v", err)
		}
		log.Println("youtube_videos 테이블이 생성되었습니다.")
		return nil
	}

	// 기존 테이블에 필요한 컬럼이 없으면 추가
	columns := map[string]string{
		"thumbnail":                      "VARCHAR(500)",
		"kind":                           "VARCHAR(100)",
		"etag":                           "VARCHAR(255)",
		"id_kind":                        "VARCHAR(100)",
		"id_video_id":                    "VARCHAR(255)",
		"id_channel_id":                  "VARCHAR(255)",
		"id_playlist_id":                 "VARCHAR(255)",
		"snippet_published_at":           "DATETIME",
		"snippet_description":            "TEXT",
		"snippet_channel_title":          "VARCHAR(255)",
		"snippet_live_broadcast_content": "VARCHAR(50)",
		"snippet_thumbnails":             "JSON",
		"raw_search_data":                "JSON",
		"activate":                       "BOOLEAN DEFAULT TRUE",
	}

	for colName, colType := range columns {
		checkQuery := `SELECT COUNT(*) FROM information_schema.columns
		               WHERE table_schema = ? AND table_name = 'youtube_videos' AND column_name = ?`
		var colCount int
		if err := db.QueryRow(checkQuery, dbName, colName).Scan(&colCount); err != nil {
			log.Printf("[WARN] 컬럼 확인 실패 (%s): %v", colName, err)
			continue
		}
		if colCount != 0 {
			continue
		}

		alterQuery := fmt.Sprintf("ALTER TABLE youtube_videos ADD COLUMN %s %s", colName, colType)
		if _, err := db.Exec(alterQuery); err != nil {
			log.Printf("[WARN] 컬럼 추가 실패 (%s): %v", colName, err)
			continue
		}
		log.Printf("컬럼이 추가되었습니다: %s", colName)

		if colName == "activate" {
			if _, err := db.Exec("UPDATE youtube_videos SET activate = TRUE WHERE activate IS NULL"); err != nil {
				log.Printf("[WARN] 기존 데이터 activate 업데이트 실패: %v", err)
			}
		}
	}
	return nil
}

// initDatabasePool 전역 커넥션 풀을 한 번만 초기화하고, 스키마를 한 번만 점검한다.
func initDatabasePool() error {
	pool, err := sql.Open("mysql", buildDSN())
	if err != nil {
		return err
	}

	pool.SetMaxOpenConns(10)
	pool.SetMaxIdleConns(5)
	pool.SetConnMaxLifetime(5 * time.Minute)

	if err := pool.Ping(); err != nil {
		pool.Close()
		return fmt.Errorf("database connection failed: %v", err)
	}

	db = pool
	if err := ensureTableExists(); err != nil {
		return err
	}

	log.Println("데이터베이스 초기화 완료")
	return nil
}

func saveToDB(videoID, title, channelID string, duration int, thumbnail string) error {
	decodedTitle := html.UnescapeString(title)
	decodedChannelID := html.UnescapeString(channelID)
	decodedThumbnail := html.UnescapeString(thumbnail)

	query := `INSERT INTO youtube_videos (id, title, channel_id, duration, thumbnail, activate)
	          VALUES (?, ?, ?, ?, ?, TRUE)
	          ON DUPLICATE KEY UPDATE title=?, channel_id=?, duration=?, thumbnail=?, activate=TRUE`
	_, err := db.Exec(query, videoID, decodedTitle, decodedChannelID, duration, decodedThumbnail,
		decodedTitle, decodedChannelID, duration, decodedThumbnail)
	return err
}

// saveSearchResultToDB YouTube Search API 응답을 그대로 DB에 저장 (raw_search_data)
func saveSearchResultToDB(searchItem *youtube.SearchResult) error {
	rawJSON, err := json.Marshal(searchItem)
	if err != nil {
		log.Printf("[WARN] Search API 응답 JSON 변환 실패: %v", err)
		rawJSON = []byte("{}")
	}

	idVideoID, idChannelID, idPlaylistID, idKind := "", "", "", ""
	if searchItem.Id != nil {
		idVideoID = searchItem.Id.VideoId
		idChannelID = searchItem.Id.ChannelId
		idPlaylistID = searchItem.Id.PlaylistId
		idKind = searchItem.Id.Kind
	}

	snippetChannelID, snippetTitle, snippetDescription := "", "", ""
	snippetChannelTitle, snippetLiveBroadcastContent := "", ""
	var snippetPublishedAt *time.Time
	var snippetThumbnailsJSON []byte
	thumbnailURL := ""

	if searchItem.Snippet != nil {
		snippetChannelID = searchItem.Snippet.ChannelId
		snippetTitle = html.UnescapeString(searchItem.Snippet.Title)
		snippetDescription = html.UnescapeString(searchItem.Snippet.Description)
		snippetChannelTitle = html.UnescapeString(searchItem.Snippet.ChannelTitle)
		snippetLiveBroadcastContent = searchItem.Snippet.LiveBroadcastContent

		if searchItem.Snippet.PublishedAt != "" {
			if publishedAt, err := time.Parse(time.RFC3339, searchItem.Snippet.PublishedAt); err == nil {
				snippetPublishedAt = &publishedAt
			}
		}

		if searchItem.Snippet.Thumbnails != nil {
			if thumbJSON, err := json.Marshal(searchItem.Snippet.Thumbnails); err == nil {
				snippetThumbnailsJSON = thumbJSON
			}
			if searchItem.Snippet.Thumbnails.Medium != nil {
				thumbnailURL = searchItem.Snippet.Thumbnails.Medium.Url
			} else if searchItem.Snippet.Thumbnails.Default != nil {
				thumbnailURL = searchItem.Snippet.Thumbnails.Default.Url
			}
		}
	}

	videoID := idVideoID
	if videoID == "" {
		return fmt.Errorf("video ID not found in search result")
	}

	query := `INSERT INTO youtube_videos (
		id, title, channel_id, duration, thumbnail,
		kind, etag,
		id_kind, id_video_id, id_channel_id, id_playlist_id,
		snippet_published_at, snippet_description, snippet_channel_title,
		snippet_live_broadcast_content, snippet_thumbnails, raw_search_data, activate
	) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, TRUE)
	ON DUPLICATE KEY UPDATE
		title = VALUES(title),
		channel_id = VALUES(channel_id),
		duration = VALUES(duration),
		thumbnail = VALUES(thumbnail),
		kind = VALUES(kind),
		etag = VALUES(etag),
		id_kind = VALUES(id_kind),
		id_video_id = VALUES(id_video_id),
		id_channel_id = VALUES(id_channel_id),
		id_playlist_id = VALUES(id_playlist_id),
		snippet_published_at = VALUES(snippet_published_at),
		snippet_description = VALUES(snippet_description),
		snippet_channel_title = VALUES(snippet_channel_title),
		snippet_live_broadcast_content = VALUES(snippet_live_broadcast_content),
		snippet_thumbnails = VALUES(snippet_thumbnails),
		raw_search_data = VALUES(raw_search_data),
		activate = TRUE,
		updated_at = CURRENT_TIMESTAMP`

	_, err = db.Exec(query,
		videoID, snippetTitle, snippetChannelID, 0, thumbnailURL,
		searchItem.Kind, searchItem.Etag,
		idKind, idVideoID, idChannelID, idPlaylistID,
		snippetPublishedAt, snippetDescription, snippetChannelTitle,
		snippetLiveBroadcastContent, string(snippetThumbnailsJSON), string(rawJSON),
	)
	return err
}

// GetRandom DB에서 활성화된 영상 중 무작위 count개를 가져온다.
func GetRandom(count int) ([]PlayItem, error) {
	var totalCount int
	if err := db.QueryRow(`SELECT COUNT(*) FROM youtube_videos WHERE activate = TRUE`).Scan(&totalCount); err != nil {
		return nil, fmt.Errorf("failed to count videos: %v", err)
	}
	if totalCount == 0 {
		return []PlayItem{}, nil
	}
	if count > totalCount {
		count = totalCount
	}

	query := `SELECT id, title, channel_id, duration, thumbnail
	          FROM youtube_videos
	          WHERE activate = TRUE
	          ORDER BY RAND()
	          LIMIT ?`
	rows, err := db.Query(query, count)
	if err != nil {
		return nil, fmt.Errorf("failed to query random videos: %v", err)
	}
	defer rows.Close()

	var items []PlayItem
	for rows.Next() {
		var item PlayItem
		var channelTitle string // channel_id 컬럼에 실제로는 channelTitle이 저장됨 (saveToDB 참고)
		var thumbnail sql.NullString

		if err := rows.Scan(&item.ID, &item.Title, &channelTitle, &item.Duration, &thumbnail); err != nil {
			log.Printf("[ERROR] GetRandom: 행 스캔 실패: %v", err)
			continue
		}

		item.ChannelTitle = channelTitle
		if thumbnail.Valid {
			item.Thumbnail = thumbnail.String
		}
		item.LocalPath = getTargetPCMPath(channelTitle, item.Title)

		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("error iterating rows: %v", err)
	}

	return items, nil
}

// ================= 다운로드 / 변환 =================

func setConsoleUTF8() {
	setConsoleUTF8Windows()
}

func getFFmpegPath() string {
	if runtime.GOOS == "windows" {
		if _, err := exec.LookPath("ffmpeg"); err == nil {
			return "ffmpeg"
		}
		return `C:\ffmpeg\bin\ffmpeg.exe`
	}
	return "ffmpeg"
}

// convertOpusToPCM Opus 파일을 PCM으로 변환
func convertOpusToPCM(opusPath string) ([]byte, error) {
	args := []string{
		"-loglevel", "error",
		"-hide_banner",
		"-i", opusPath,
		"-vn",
		"-ar", "48000",
		"-ac", "2",
		"-channel_layout", "stereo",
		"-acodec", "pcm_s16le",
		"-f", "s16le",
		"pipe:1",
	}

	cmd := exec.Command(getFFmpegPath(), args...)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}

	go func() { io.Copy(io.Discard, stderr) }()

	pcmData, err := io.ReadAll(stdout)
	if err != nil {
		return nil, err
	}
	if err := cmd.Wait(); err != nil {
		return nil, err
	}
	return pcmData, nil
}

// downloadAndConvert 영상을 opus로 받아 pcm으로 변환해 targetPath에 저장한다.
func downloadAndConvert(videoURL, targetPath string) error {
	if err := os.MkdirAll(filepath.Dir(targetPath), 0755); err != nil {
		return err
	}

	opusPath := strings.TrimSuffix(targetPath, ".pcm") + ".opus"

	playerClients := []string{"android", "ios", "web"}
	var lastErr error
	downloadSuccess := false

	for _, client := range playerClients {
		args := []string{
			"-x",
			"--audio-format", "opus",
			"--audio-quality", "0",
			"--no-playlist",
			"--no-part",
			"--no-mtime",
			"--extractor-args", fmt.Sprintf("youtube:player_client=%s", client),
			"--no-warnings",
			"-o", opusPath,
			videoURL,
		}

		log.Printf("[yt-dlp] 실행 명령어 (player_client=%s): yt-dlp %s", client, strings.Join(args, " "))
		out, err := exec.Command("yt-dlp", args...).CombinedOutput()
		if err != nil {
			lastErr = fmt.Errorf("download failed (player_client=%s): %v, output: %s", client, err, string(out))
			log.Printf("[yt-dlp] 실패 (player_client=%s), 다음 클라이언트 시도...", client)
			continue
		}

		log.Printf("[yt-dlp] 다운로드 성공 (player_client=%s)", client)
		downloadSuccess = true
		break
	}

	if !downloadSuccess {
		return fmt.Errorf("모든 player client 시도 실패. 마지막 에러: %v", lastErr)
	}
	defer os.Remove(opusPath)

	pcmData, err := convertOpusToPCM(opusPath)
	if err != nil {
		return err
	}
	return os.WriteFile(targetPath, pcmData, 0644)
}

// ensureDownloaded targetPath가 없으면 다운로드한다.
// 같은 targetPath에 대한 동시 요청은 downloadLocks로 직렬화되어 중복 다운로드를 막는다.
func ensureDownloaded(videoURL, targetPath string) error {
	unlock := downloadLocks.Lock(targetPath)
	defer unlock()

	if _, err := os.Stat(targetPath); err == nil {
		return nil // 락을 잡은 사이 다른 요청이 이미 받아둔 경우 포함
	}
	return downloadAndConvert(videoURL, targetPath)
}

// ================= 비디오 메타데이터 =================

// videoMeta YouTube Videos API로 조회한 단일 영상 메타데이터
type videoMeta struct {
	ID           string
	Title        string
	ChannelID    string
	ChannelTitle string
	Thumbnail    string
	Duration     int
}

func fetchVideoMeta(videoID string) (*videoMeta, error) {
	resp, err := youtubeService.Videos.List([]string{"snippet", "contentDetails"}).Id(videoID).Do()
	if err != nil {
		return nil, fmt.Errorf("failed to get video details: %v", err)
	}
	if len(resp.Items) == 0 {
		return nil, fmt.Errorf("video not found: %s", videoID)
	}

	v := resp.Items[0]
	meta := &videoMeta{
		ID:           v.Id,
		Title:        html.UnescapeString(v.Snippet.Title),
		ChannelID:    v.Snippet.ChannelId,
		ChannelTitle: html.UnescapeString(v.Snippet.ChannelTitle),
	}
	if v.Snippet.Thumbnails != nil && v.Snippet.Thumbnails.Medium != nil {
		meta.Thumbnail = v.Snippet.Thumbnails.Medium.Url
	}
	if v.ContentDetails != nil && v.ContentDetails.Duration != "" {
		meta.Duration = parseISO8601Duration(v.ContentDetails.Duration)
	}
	return meta, nil
}

// searchVideoByURL 비디오 URL/ID로 상세 정보를 가져와 PlayItem으로 변환한다 (플레이리스트 아이템용).
func searchVideoByURL(videoURL string) (*PlayItem, error) {
	videoID := extractVideoID(videoURL)
	if videoID == "" {
		videoID = videoURL
	}

	meta, err := fetchVideoMeta(videoID)
	if err != nil {
		return nil, err
	}

	return &PlayItem{
		ID:           meta.ID,
		Title:        meta.Title,
		ChannelID:    meta.ChannelID,
		ChannelTitle: meta.ChannelTitle,
		Thumbnail:    meta.Thumbnail,
		LocalPath:    getTargetPCMPath(meta.ChannelTitle, meta.Title),
		Duration:     meta.Duration,
	}, nil
}

// ================= HTTP 핸들러 =================

// resolveVideoID 요청 입력(URL 또는 검색어)에서 처리할 videoID를 결정한다.
// 검색어인 경우 YouTube Search API를 호출하고, 결과를 searchResults/DB에 남긴다.
func resolveVideoID(input string) (videoID string, searchResults []VideoResult, err error) {
	if isYoutubeURL(input) {
		videoID = extractVideoID(input)
		if videoID == "" {
			// v=/youtu.be 패턴이 아니면 입력 자체를 ID로 간주
			videoID = input
		}
		return videoID, nil, nil
	}

	// 검색어로 간주하고 YouTube Search API 호출
	resp, searchErr := youtubeService.Search.List([]string{"id", "snippet"}).
		Q(input).
		MaxResults(5).
		Type("video").
		Do()
	if searchErr != nil {
		return "", nil, fmt.Errorf("search failed: %v", searchErr)
	}

	for _, item := range resp.Items {
		searchResults = append(searchResults, VideoResult{
			ID:    item.Id.VideoId,
			Title: html.UnescapeString(item.Snippet.Title),
		})
	}
	if len(searchResults) == 0 {
		return "", nil, fmt.Errorf("no results found")
	}

	videoID = searchResults[0].ID
	if err := saveSearchResultToDB(resp.Items[0]); err != nil {
		log.Printf("[WARN] Search 결과 DB 저장 실패 (videoId: %s): %v", resp.Items[0].Id.VideoId, err)
	}
	return videoID, searchResults, nil
}

// handleProcess 요청 하나를 아래 순서로만 처리한다:
//  1. start_radio 거부
//  2. 플레이리스트 판별 → 맞으면 handlePlaylist로 위임하고 종료
//  3. videoID 결정 (URL 파싱 또는 검색)
//  4. 메타데이터 조회 → DB 저장 → 필요시 다운로드
func handleProcess(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	enc := json.NewEncoder(w)

	query := r.URL.Query().Get("q")
	urlInput := r.URL.Query().Get("url")

	input := urlInput
	if input == "" {
		input = query
	}
	if input == "" {
		enc.Encode(JSONResponse{Success: false, Error: "Missing query or url parameter"})
		return
	}
	log.Printf("[DEBUG] 요청 수신: %s", input)

	if hasStartRadio(input) {
		enc.Encode(JSONResponse{Success: false, Error: "start_radio playlists are not supported"})
		return
	}

	// 플레이리스트가 감지되면 (watch?v=가 같이 있어도) 플레이리스트로 처리
	if playlistID := extractPlaylistID(input); playlistID != "" {
		log.Printf("[DEBUG] 플레이리스트 감지: %s", playlistID)
		handlePlaylist(w, playlistID)
		return
	}

	videoID, searchResults, err := resolveVideoID(input)
	if err != nil {
		enc.Encode(JSONResponse{Success: false, Error: err.Error()})
		return
	}

	meta, err := fetchVideoMeta(videoID)
	if err != nil {
		enc.Encode(JSONResponse{Success: false, Error: "Failed to get video details"})
		return
	}

	targetPath := getTargetPCMPath(meta.ChannelTitle, meta.Title)

	dbStatus := "Success"
	if err := saveToDB(meta.ID, meta.Title, meta.ChannelTitle, meta.Duration, meta.Thumbnail); err != nil {
		dbStatus = "DB Save Failed: " + err.Error()
	}

	response := JSONResponse{
		Success: true,
		Metadata: map[string]interface{}{
			"id":           meta.ID,
			"title":        meta.Title,
			"channelId":    meta.ChannelID,
			"channelTitle": meta.ChannelTitle,
			"thumbnail":    meta.Thumbnail,
			"duration":     meta.Duration,
		},
		SearchResults: searchResults,
		LocalPath:     targetPath,
		DBStatus:      dbStatus,
	}

	videoURL := "https://www.youtube.com/watch?v=" + meta.ID
	if err := ensureDownloaded(videoURL, targetPath); err != nil {
		log.Printf("[DEBUG] 다운로드/변환 실패: %v", err)
		response.Success = false
		response.Error = "Download/Convert failed: " + err.Error()
	}

	enc.Encode(response)
}

// handlePlaylist 플레이리스트의 모든 비디오를 가져와서 다운로드/저장 후 JSON으로 반환
func handlePlaylist(w http.ResponseWriter, playlistID string) {
	w.Header().Set("Content-Type", "application/json")
	enc := json.NewEncoder(w)

	validIDReg := regexp.MustCompile(`^[a-zA-Z0-9_-]+$`)
	if !validIDReg.MatchString(playlistID) || len(playlistID) < 13 || len(playlistID) > 50 {
		log.Printf("[ERROR] 잘못된 플레이리스트 ID: %s", playlistID)
		enc.Encode(map[string]interface{}{
			"success": false,
			"error":   fmt.Sprintf("잘못된 플레이리스트 ID입니다: %s", playlistID),
		})
		return
	}

	log.Printf("[DEBUG] 플레이리스트 처리 시작: %s", playlistID)

	var playlistItems []PlayItem
	nextPageToken := ""

	for {
		call := youtubeService.PlaylistItems.List([]string{"snippet"}).
			PlaylistId(playlistID).
			MaxResults(50)
		if nextPageToken != "" {
			call = call.PageToken(nextPageToken)
		}

		resp, err := call.Do()
		if err != nil {
			log.Printf("[ERROR] 플레이리스트 아이템 가져오기 실패 (%s): %v", playlistID, err)
			enc.Encode(map[string]interface{}{
				"success": false,
				"error":   describePlaylistError(playlistID, err),
			})
			return
		}

		for _, playlistItem := range resp.Items {
			if playlistItem.Snippet == nil || playlistItem.Snippet.ResourceId == nil {
				continue
			}
			videoID := playlistItem.Snippet.ResourceId.VideoId
			if videoID == "" {
				continue
			}

			videoURL := "https://www.youtube.com/watch?v=" + videoID
			item, err := searchVideoByURL(videoURL)
			if err != nil {
				log.Printf("[WARN] 비디오 상세 정보 가져오기 실패 (%s): %v", videoID, err)
				continue
			}
			playlistItems = append(playlistItems, *item)
		}

		nextPageToken = resp.NextPageToken
		if nextPageToken == "" {
			break
		}
	}

	log.Printf("[DEBUG] 플레이리스트 아이템 %d개 확인, 다운로드 시작", len(playlistItems))

	successCount, failCount := 0, 0
	for i, item := range playlistItems {
		videoURL := "https://www.youtube.com/watch?v=" + item.ID
		if err := ensureDownloaded(videoURL, item.LocalPath); err != nil {
			log.Printf("[ERROR] [%d/%d] 다운로드/변환 실패: %s - %v", i+1, len(playlistItems), item.Title, err)
			failCount++
			continue
		}

		if err := saveToDB(item.ID, item.Title, item.ChannelTitle, item.Duration, item.Thumbnail); err != nil {
			log.Printf("[ERROR] [%d/%d] DB 저장 실패: %s - %v", i+1, len(playlistItems), item.Title, err)
			failCount++
			continue
		}
		successCount++
	}

	log.Printf("[DEBUG] 플레이리스트 처리 완료: 성공 %d개, 실패 %d개", successCount, failCount)

	// 클라이언트(C++)는 이 엔드포인트 응답에서 "list" 키를 읽는다 (하위 호환 유지)
	enc.Encode(map[string]interface{}{
		"success":       true,
		"list":          playlistItems,
		"total":         len(playlistItems),
		"success_count": successCount,
		"failed_count":  failCount,
	})
}

func describePlaylistError(playlistID string, err error) string {
	errStr := err.Error()
	switch {
	case strings.Contains(errStr, "404") || strings.Contains(errStr, "playlistNotFound"):
		return fmt.Sprintf("플레이리스트를 찾을 수 없습니다 (ID: %s).", playlistID)
	case strings.Contains(errStr, "playlistOperationUnsupported"):
		return fmt.Sprintf("이 플레이리스트는 YouTube API로 접근할 수 없습니다 (ID: %s).", playlistID)
	case strings.Contains(errStr, "400") || strings.Contains(errStr, "invalid"):
		return fmt.Sprintf("플레이리스트 ID가 유효하지 않습니다 (ID: %s).", playlistID)
	case strings.Contains(errStr, "403") || strings.Contains(errStr, "playlistItemsNotAccessible"):
		return fmt.Sprintf("플레이리스트에 접근할 권한이 없습니다 (ID: %s).", playlistID)
	default:
		return fmt.Sprintf("플레이리스트 가져오기 실패 (ID: %s): %v", playlistID, err)
	}
}

func handleGetRandom(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	enc := json.NewEncoder(w)

	countStr := r.URL.Query().Get("count")
	if countStr == "" {
		countStr = "10"
	}
	count, err := strconv.Atoi(countStr)
	if err != nil || count <= 0 {
		enc.Encode(map[string]interface{}{
			"success": false,
			"error":   "Invalid count parameter. Must be a positive integer.",
		})
		return
	}
	if count > 100 {
		count = 100
	}

	items, err := GetRandom(count)
	if err != nil {
		log.Printf("[ERROR] GetRandom 실패: %v", err)
		enc.Encode(map[string]interface{}{"success": false, "error": err.Error()})
		return
	}

	enc.Encode(map[string]interface{}{
		"success": true,
		"count":   len(items),
		"items":   items,
	})
}

func handleDeactivate(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	enc := json.NewEncoder(w)

	videoID := r.URL.Query().Get("id")
	if videoID == "" {
		enc.Encode(map[string]interface{}{"success": false, "error": "Missing id parameter"})
		return
	}

	result, err := db.Exec(`UPDATE youtube_videos SET activate = FALSE WHERE id = ?`, videoID)
	if err != nil {
		log.Printf("[ERROR] Deactivate: 업데이트 실패 (id: %s): %v", videoID, err)
		enc.Encode(map[string]interface{}{"success": false, "error": "Update failed: " + err.Error()})
		return
	}

	rowsAffected, err := result.RowsAffected()
	if err != nil {
		enc.Encode(map[string]interface{}{"success": false, "error": "Failed to get rows affected: " + err.Error()})
		return
	}
	if rowsAffected == 0 {
		enc.Encode(map[string]interface{}{"success": false, "error": "Video not found: " + videoID})
		return
	}

	log.Printf("[DEBUG] Deactivate: 비활성화 완료 (id: %s)", videoID)
	enc.Encode(map[string]interface{}{"success": true, "message": "Video deactivated successfully", "id": videoID})
}

// ================= 엔트리포인트 =================

// resolveBindAddress 설정에 호스트가 없으면 로컬(127.0.0.1)에만 바인딩한다.
func resolveBindAddress() string {
	port := appConfig.Port
	if port == "" {
		return "127.0.0.1:8080"
	}
	if strings.HasPrefix(port, ":") {
		return "127.0.0.1" + port
	}
	if !strings.Contains(port, ":") {
		return "127.0.0.1:" + port
	}
	return port // 이미 host:port 형태로 명시된 경우 그대로 사용
}

func main() {
	setConsoleUTF8()

	if err := loadConfig(); err != nil {
		log.Fatalf("Failed to load config: %v", err)
	}
	if err := initYouTubeService(); err != nil {
		log.Fatalf("Failed to init YouTube service: %v", err)
	}
	if err := initDatabasePool(); err != nil {
		log.Fatalf("Failed to init database: %v", err)
	}
	defer db.Close()

	http.HandleFunc("/process", handleProcess)
	http.HandleFunc("/getrandom", handleGetRandom)
	http.HandleFunc("/deactivate", handleDeactivate)

	addr := resolveBindAddress()
	fmt.Printf("Server starting on http://%s\n", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("Server failed: %v", err)
	}
}

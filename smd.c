#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <libspotify/api.h>

#include <libspotify/api.h>
#define STATE_SHUTDOWN  0
#define STATE_STARTED   1
#define STATE_CONNECTED 2
#define STATE_READY     3

#include "audio.h"
#include "keys.h"

struct track {
  sp_track *track;
  struct track *next;
};

struct event {
  int type;
  char *data;
  struct event *next;
};

/* Global state */
static sp_session     *session;
static int             state;

static int             qlen;
static struct track   *track_queue;
static struct event   *event_queue;
static sp_track       *current_track;
static time_t          stamp;


/* Main thread notification structure */
static int             notify_events;
static pthread_mutex_t notify_mutex;
static pthread_cond_t  notify_cond;

/* Server */
static int             socket_fd;
static char            socket_buf[1024];

FILE *log_fd;

#define QUIT   0
#define QUEUE  1
#define PUSH   2
#define NEXT   3
#define CLEAR  4
#define STATUS 5

#define CRED_FILE "tmp/creds"

/*
 * =============================================================================
 * API
 * =============================================================================
 */
static int play_track()
{
  sp_error err;

  if (current_track) {
    err = sp_session_player_load(session, current_track);
    if (err == SP_ERROR_OK) {
      fprintf(log_fd, "Playing track: %s\n", sp_track_name(current_track));
      sp_session_player_play(session, 1);
      stamp = time(NULL);
    } else {
      fprintf(log_fd, "Failed to load: %s\n", sp_track_name(current_track));
      current_track = NULL;
      return -1;
    }
  } else {
    fprintf(log_fd, "No track set\n");
    return -1;
  }

  return 0;
}

void next_track()
{
  struct track *t;

  sp_session_player_play(session, 0);
  audio_flush();

  do {
    if (!track_queue) {
      current_track = NULL;
      stamp = 0;
      return;
    }

    t = track_queue;
    track_queue = t->next;

    if (current_track)
      sp_track_release(current_track);

    current_track = t->track;
    free(t);

    --qlen;
  } while (play_track() == -1);
}

void clear_queue()
{
  struct track *t;

  sp_session_player_play(session, 0);
  while (track_queue) {
    t = track_queue;
    track_queue = track_queue->next;

    sp_track_release(t->track);
    free(t);
  }

  qlen = 0;
}

void push_track(sp_track *track)
{
  struct track *q;

  q = malloc(sizeof(struct track));
  if (!q)
    abort();

  sp_track_add_ref(track);
  q->track = track;
  q->next = track_queue;
  track_queue = q;

  ++qlen;

  if (!current_track)
    next_track();
}

void queue_track(sp_track *track)
{
  struct track *q, *qp;

  q = malloc(sizeof(struct track));
  if (!q)
    abort();

  sp_track_add_ref(track);
  q->track = track;
  q->next = NULL;

  ++qlen;

  if (!track_queue) {
    track_queue = q;
  } else {
    qp = track_queue;
    while (qp->next)
      qp = qp->next;
    qp->next = q;
  }

  if (!current_track)
    next_track();
}

void push_playlist(sp_playlist *pl)
{
  int i, n;
  sp_track *track;

  if (!sp_playlist_is_loaded(pl))
    return;

  n = sp_playlist_num_tracks(pl);
  for (i = 0; i < n; ++i) {
    track = sp_playlist_track(pl, i);
    push_track(track);
  }
}

void queue_playlist(sp_playlist *pl)
{
  int i, n;
  sp_track *track;

  if (!sp_playlist_is_loaded(pl))
    return;

  n = sp_playlist_num_tracks(pl);
  for (i = 0; i < n; ++i) {
    track = sp_playlist_track(pl, i);
    queue_track(track);
  }
}

int queue_link(const char *uri)
{
  sp_link *l;
  sp_playlist *pl;
  sp_track *t;

  int r = 0;

  l = sp_link_create_from_string(uri);
  if (!l)
    return -1;

  switch (sp_link_type(l)) {
  case SP_LINKTYPE_PLAYLIST:
    pl = sp_playlist_create(session, l);
    if (pl && sp_playlist_is_loaded(pl))
      queue_playlist(pl);
    else
      r = -1;
    break;
  case SP_LINKTYPE_TRACK:
    t = sp_link_as_track(l);
    if (t && sp_track_is_loaded(t))
      queue_track(t);
    else
      r = -1;
    break;
  default:
    break;
  }

  sp_link_release(l);
  return r;
}

int push_link(const char *uri)
{
  sp_link *l;
  sp_playlist *pl;
  sp_track *t;

  int r = 0;

  l = sp_link_create_from_string(uri);
  if (!l)
    return -1;

  switch (sp_link_type(l)) {
  case SP_LINKTYPE_PLAYLIST:
    pl = sp_playlist_create(session, l);
    if (pl && sp_playlist_is_loaded(pl))
      push_playlist(pl);
    else
      r = -1;
    break;
  case SP_LINKTYPE_TRACK:
    t = sp_link_as_track(l);
    if (t && sp_track_is_loaded(t))
      push_track(t);
    else
      r = -1;
    break;
  default:
    break;
  }

  sp_link_release(l);
  return r;
}

int format_current_track(char *buf, int len)
{
      sp_artist *a;
      int t;

      t = time(NULL) - stamp;
      a = sp_track_artist(current_track, 0);
      return sprintf(buf, "%s - %s %02d:%02d",
                     sp_artist_name(a),
                     sp_track_name(current_track),
                     t / 60, t % 60);
}

/*
 * =============================================================================
 * Utils
 * =============================================================================
 */
char *cache_dir()
{
  struct stat st;
  char buf[1024];

  sprintf(buf, "%s/.cache/smd", getenv("HOME"));
  if (stat(buf, &st) != 0)
    mkdir(buf, 0777);

  return strdup(buf);
}

char *load_blob()
{
  char filename[512];
  char buf[1024];

  FILE *fd;
  struct stat st;

  sprintf(filename, "%s/.cache/smd/creds", getenv("HOME"));
  if (stat(filename, &st) == 0) {
    fd = fopen(filename, "r");
    if (fd) {
      buf[st.st_size] = '\0';

      fread(buf, 1, st.st_size, fd);
      fclose(fd);

      return strdup(buf);
    }
  }

  return NULL;
}

/*
 * =============================================================================
 * Server
 * =============================================================================
 */
static int server_start()
{
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1025);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  if (bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) < 0) {
    perror("bind");
    exit(EXIT_FAILURE);
  }

  fprintf(log_fd, "Server started\n");
  return fd;
}

static int server_recv(int fd, char **payload, int *len, struct sockaddr *addr, socklen_t *addrlen)
{
  int l;

  l = recvfrom(fd, &socket_buf, 1024, MSG_DONTWAIT, addr, addrlen);
  if (l <= 0)
    return -1;

  *len = socket_buf[1] << 8 | socket_buf[2];
  if (*len > l || *len >= 1020)
    return -1;

  socket_buf[*len + 3] = '\0';
  *payload = socket_buf + 3;

  fprintf(log_fd, "Recvd: %d %s\n", socket_buf[0], socket_buf + 3);
  fflush(log_fd);

  return socket_buf[0];
}

static int server_send(int fd, char type, char *payload, int len, struct sockaddr *addr, socklen_t addrlen)
{
  socket_buf[0] = type;
  socket_buf[1] = (char) ((len >> 8) & 0xFF00);
  socket_buf[2] = (char) (len & 0xFF);

  if (len)
    strncpy(socket_buf + 3, payload, len);

  fprintf(log_fd, "Send: %d %s\n", type, payload);
  fflush(log_fd);

  return sendto(fd, socket_buf, len + 3, 0, addr, addrlen);
}

static void server_handle_event(int fd)
{
  struct event *event, *ep;

  struct sockaddr addr;
  socklen_t addrlen;

  char buf[1000], *payload;
  int len = 0;

  int cmd = server_recv(fd, &payload, &len, &addr, &addrlen);
  switch (cmd) {
  case QUIT:
    state = STATE_SHUTDOWN;
    break;

  case STATUS:
    if (current_track) {
      len = format_current_track(buf, 1000);
      server_send(fd, 0, buf, len, &addr, addrlen);
    } else {
      server_send(fd, 0, "stopped", 7, &addr, addrlen);
    }
    break;

  case CLEAR:
    clear_queue();
    while (event_queue) {
      ep = event_queue;
      event_queue = event_queue->next;
      free(ep->data);
      free(ep);
    }
    break;

  case QUEUE:
  case PUSH:
  case NEXT:
    event = malloc(sizeof(struct event));
    event->type = cmd;
    event->data = len ? strdup(payload) : NULL;
    event->next = NULL;

    if (!event_queue) {
      event_queue = event;
    } else {
      for (ep = event_queue; ep->next; ep = ep->next)
        ;
      ep->next = event;
    }
  }
}

static void server_process_events()
{
  struct event *event = event_queue;

  if (!event)
    return;

  switch (event->type) {
  case QUEUE:
    if (queue_link(event->data) == 0) {
      event_queue = event->next;
      free(event->data);
      free(event);
    }
    break;

  case PUSH:
    if (push_link(event->data) == 0) {
      event_queue = event->next;
      free(event->data);
      free(event);
    }
    break;

  case NEXT:
    next_track();
    event_queue = event->next;
    free(event->data);
    break;

  default:
    break;
  }
}

/*
 * =============================================================================
 * Spotify
 * =============================================================================
 */

static void playlist_state_changed(sp_playlist *pl, void *userdata)
{
  if (sp_playlist_is_loaded(pl))
    fprintf(log_fd, "Playlist Loaded: %s\n", sp_playlist_name(pl));
}

static void playlist_added(sp_playlistcontainer *pc, sp_playlist *pl, int p, void *userdata)
{
  static sp_playlist_callbacks callbacks = {
    .playlist_state_changed = &playlist_state_changed
  };

  sp_playlist_add_callbacks(pl, &callbacks, NULL);
}

static void playlist_removed(sp_playlistcontainer *pc, sp_playlist *pl, int p, void *userdata)
{
  fprintf(log_fd, "Playlist removed: %s\n", sp_playlist_name(pl));
}

static void container_loaded(sp_playlistcontainer *pc, void *userdata)
{
  if (state != STATE_READY) {
    state = STATE_READY;
    fprintf(log_fd, "Ready!\n");
    fprintf(log_fd, "Found %d playlists\n", sp_playlistcontainer_num_playlists(pc));
    fflush(log_fd);
  }
}

static void credentials_blob_updated(sp_session *session, const char *blob)
{
  FILE *fd;
  char filename[512];

  sprintf(filename, "%s/.cache/smd/creds", getenv("HOME"));

  fd = fopen(filename, "w");
  if (fd) {
    if (fputs(blob, fd) == EOF)
      fprintf(log_fd, "Failed to update blob\n");
    fclose(fd);
  } else {
    fprintf(log_fd, "Failed to update blob\n");
  }

}

static void connectionstate_updated(sp_session *session)
{
  int cs;
  sp_playlistcontainer *pc;

  static sp_playlistcontainer_callbacks callbacks = {
    .playlist_added   = &playlist_added,
    .playlist_removed = &playlist_removed,
    .container_loaded = &container_loaded
  };

  cs = sp_session_connectionstate(session);
  fprintf(log_fd, "Connection State: %d\n", cs);
  fflush(log_fd);

  if (cs == SP_CONNECTION_STATE_LOGGED_IN) {
    pc = sp_session_playlistcontainer(session);
    if (pc) {
      sp_playlistcontainer_add_callbacks(pc, &callbacks, NULL);
      if (sp_playlistcontainer_is_loaded(pc))
        container_loaded(pc, NULL);
    }
  } else {
    state = STATE_CONNECTED;
  }

}

static void message_to_user(sp_session *session, const char *msg)
{
  fprintf(log_fd, "Message to user: %s\n", msg);
}

static void connection_error(sp_session *session, sp_error error)
{
  fprintf(log_fd, "Connection error: %s\n", sp_error_message(error));
}

static void logger(sp_session *session, const char *data)
{
  fprintf(log_fd, "Log: %s", data);
  fflush(log_fd);
}

static void finish(int sig)
{
  sp_session_logout(session);
  audio_stop();

  close(socket_fd);
  fclose(log_fd);

  exit(0);
}

static void notify_main_thread(sp_session *session)
{
  pthread_mutex_lock(&notify_mutex);
  notify_events = 1;
  pthread_cond_signal(&notify_cond);
  pthread_mutex_unlock(&notify_mutex);
}

static void get_audio_buffer_stats(sp_session *session, sp_audio_buffer_stats *stats)
{
  stats->samples = audio_buffered();
  stats->stutter = 0;
}

static int music_delivery(sp_session *session, const sp_audioformat *format, const void *frames, int num_frames)
{
  return audio_push(frames, num_frames, format->sample_rate, format->channels, 16);
}

static void end_of_track(sp_session *session)
{
  next_track();
}

static int process_events(sp_session *session)
{
  int next_timeout = 0;

  do {
    pthread_mutex_unlock(&notify_mutex);
    sp_session_process_events(session, &next_timeout);
    pthread_mutex_lock(&notify_mutex);
  } while (next_timeout == 0);

  notify_events = 0;
  return next_timeout;
}

static void main_loop()
{
  int next_timeout;

  struct pollfd fds[1];
  fds[0].fd = socket_fd;
  fds[0].events = POLLIN;

  audio_start();
  pthread_mutex_lock(&notify_mutex);

  state = STATE_STARTED;
  next_timeout = 0;
  while (state) {
    if (next_timeout || notify_events)
      next_timeout = process_events(session);

    pthread_mutex_unlock(&notify_mutex);

    if (next_timeout == 0 || next_timeout > 200)
      next_timeout = 200;

    fds[0].revents = 0;
    if (poll(fds, 1, next_timeout) > 0)
      server_handle_event(socket_fd);

    if (state == STATE_READY)
      server_process_events();

    pthread_mutex_lock(&notify_mutex);
  }

  pthread_mutex_unlock(&notify_mutex);
  audio_stop();
}

int main(int argc, char **argv)
{
  sp_error err;

  char *username, *password;
  char *blob;
  char *cachepath;

  pthread_cond_init(&notify_cond, NULL);
  pthread_mutex_init(&notify_mutex, NULL);

  cachepath = cache_dir();
  blob = load_blob();

  if (argc < 2) {
    fprintf(stderr, "%s username [password]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if (!blob && argc < 3) {
    fprintf(stderr, "%s username [password]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  username = argv[1];
  password = blob ? NULL : argv[2];

  log_fd = fopen("log", "w");
  cache_dir(cachepath);

  audio_init();
  signal(SIGINT, finish);

  notify_events = 0;

  sp_session_callbacks session_callbacks = {
    .notify_main_thread       = &notify_main_thread,
    .music_delivery           = &music_delivery,
    .get_audio_buffer_stats   = &get_audio_buffer_stats,
    .end_of_track             = &end_of_track,
    .log_message              = &logger,
    .message_to_user          = &message_to_user,
    .connection_error         = &connection_error,
    .connectionstate_updated  = &connectionstate_updated,
    .credentials_blob_updated = &credentials_blob_updated
  };

  sp_session_config config = {
    .api_version          = SPOTIFY_API_VERSION,
    .cache_location       = cachepath,
    .settings_location    = cachepath,
    .application_key      = g_appkey,
    .application_key_size = 0,
    .user_agent           = "spotify music deamon",
    .callbacks            = &session_callbacks,
    NULL,
  };
  config.application_key_size = g_appkey_size;

  err = sp_session_create(&config, &session);
  if (err != SP_ERROR_OK) {
    printf("Failed to create session\n");
    exit(EXIT_FAILURE);
  }


  err = sp_session_login(session, username, password, 1, blob);
  if (err != SP_ERROR_OK) {
    printf("Failed to log in\n");
    exit(EXIT_FAILURE);
  }

  socket_fd = server_start();

  main_loop();

  pthread_mutex_unlock(&notify_mutex);
  finish(0);

  return 0;
}

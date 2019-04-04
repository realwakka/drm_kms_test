#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>


#include <iostream>
#include <tuple>
#include <string.h>
#include <vector>
#include <drm.h>

#include <gbm.h>
#include <qxl_drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

class Connector {
public:
  Connector(uint32_t conn_id, uint32_t encoder_id, uint32_t crtc_id) : conn_id_{conn_id}, encoder_id_{encoder_id}, crtc_id_{crtc_id} {}

  uint32_t conn_id_;
  uint32_t encoder_id_;
  uint32_t crtc_id_;
 
  uint32_t width_;
  uint32_t height_;
};

class Buffer {
 public:
  int buffer_handle_;
  uint32_t width_;
  uint32_t height_;
  uint32_t stride_;
  uint32_t offset_;
  
  void* buffer_map_;
  
  struct gbm_device* gbm_;
  struct gbm_bo* bo_;
  
  EGLDisplay display;
  EGLContext context;
  EGLConfig config;
  GLuint gl_tex;
  GLuint gl_fb;
  // GLuint fb_, color_rb_, depth_rb_;
  EGLImageKHR image_;
  PFNEGLCREATEIMAGEKHRPROC CreateImageKHR;
  PFNEGLDESTROYIMAGEKHRPROC DestroyImageKHR;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC EGLImageTargetTexture2DOES;
  PFNEGLCREATESYNCKHRPROC CreateSyncKHR;
  PFNEGLCLIENTWAITSYNCKHRPROC ClientWaitSyncKHR;

  bool egl_sync_supported;
};

std::vector<Connector> conn_list;
Buffer buffer;

bool init_egl(int fd, Connector& conn)
{
  buffer.CreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  buffer.DestroyImageKHR =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  buffer.EGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  buffer.CreateSyncKHR =
      (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
  buffer.ClientWaitSyncKHR =
      (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");

  if (!buffer.CreateImageKHR || !buffer.DestroyImageKHR ||
      !buffer.EGLImageTargetTexture2DOES) {
    fprintf(
        stderr,
        "eglGetProcAddress returned nullptr for a required extension entry "
        "point.\n");
    return false;
  }
  if (buffer.CreateSyncKHR && buffer.ClientWaitSyncKHR) {
    buffer.egl_sync_supported = true;
  } else {
    buffer.egl_sync_supported = false;
  }

  buffer.gbm_ = gbm_create_device (fd);
  buffer.display = eglGetDisplay(buffer.gbm_);

  EGLint major, minor = 0;
  if (!eglInitialize(buffer.display, &major, &minor)) {
    fprintf(stderr, "failed to initialize\n");
    return false;
  }

  printf("Using display %p with EGL version %d.%d\n", buffer.display, major,
         minor);

  printf("EGL Version \"%s\"\n", eglQueryString(buffer.display, EGL_VERSION));
  printf("EGL Vendor \"%s\"\n", eglQueryString(buffer.display, EGL_VENDOR));

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    fprintf(stderr, "failed to bind api EGL_OPENGL_ES_API\n");
    return false;
  }

  static const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_DONT_CARE,
                                          EGL_NONE};
  EGLint num_config = 0;
  if (!eglChooseConfig(buffer.display, config_attribs, &buffer.config, 1,
                       &num_config) ||
      num_config != 1) {
    fprintf(stderr, "failed to choose config: %d\n", num_config);
    return false;
  }

  static const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  buffer.context = eglCreateContext(buffer.display, buffer.config, EGL_NO_CONTEXT,
                                  context_attribs);
  if (buffer.context == nullptr) {
    fprintf(stderr, "failed to create context\n");
    return false;
  }
  /* connect the context to the surface */
  if (!eglMakeCurrent(
          buffer.display, EGL_NO_SURFACE /* no default draw surface */,
          EGL_NO_SURFACE /* no default draw read */, buffer.context)) {
    // fprintf(stderr, "failed to make the OpenGL ES Context current: %s\n",
    //         EglGetError());
    return false;
  }

  const std::string egl_extensions = eglQueryString(buffer.display, EGL_EXTENSIONS);
  // printf("EGL Extensions \"%s\"\n", egl_extensions);
  if (egl_extensions.find("EGL_KHR_image_base") == std::string::npos) {
    fprintf(stderr, "EGL_KHR_image_base extension not supported\n");
    return false;
  }
  if (egl_extensions.find("EGL_EXT_image_dma_buf_import") == std::string::npos) {
    fprintf(stderr, "EGL_EXT_image_dma_buf_import extension not supported\n");
    return false;
  }

  const std::string gl_extensions = (const char*)glGetString(GL_EXTENSIONS);
  if (gl_extensions.find("GL_OES_EGL_image") == std::string::npos) {
    fprintf(stderr, "GL_OES_EGL_image extension not supported\n");
    return false;
  }

  return true;
}

auto find_crtc(int fd, drmModeRes *res, drmModeConnector *conn) {
  if (conn->encoder_id) {
    auto enc = drmModeGetEncoder(fd, conn->encoder_id);
    if (enc->crtc_id) {
      auto crtc = enc->crtc_id;
      for (auto&& conn : conn_list) {
	if (conn.crtc_id_ == enc->crtc_id) {
	  crtc = 0;
	  break;
	}
      }

      if (crtc != 0) {
	conn_list.emplace_back(conn->connector_id, enc->encoder_id, crtc);
        conn_list.back().width_ = conn->modes[0].hdisplay;
        conn_list.back().height_ = conn->modes[0].vdisplay;
	drmModeFreeEncoder(enc);
	return true;
      }
    }
    drmModeFreeEncoder(enc);
    return false;
    
  } else {
    
    for (auto i = 0; i < conn->count_encoders; ++i) {
      auto enc = drmModeGetEncoder(fd, conn->encoders[i]);
      for (auto j = 0; j < res->count_crtcs; ++j) {
	if (!(enc->possible_crtcs & (1 << j)))
	  continue;

	auto crtc = res->crtcs[j];
	for (auto&& conn : conn_list) {
	  if (conn.crtc_id_ == enc->crtc_id) {
	    crtc = 0;
	    break;
	  }
	}
	
	if (crtc > 0) {
          conn_list.emplace_back(conn->connector_id, enc->encoder_id, crtc);
          conn_list.back().width_ = conn->modes[0].hdisplay;
          conn_list.back().height_ = conn->modes[0].vdisplay;
	  drmModeFreeEncoder(enc);
          return true;
	}
      }
    }

    
  }
  
  return false;
}



auto prepare(int fd) {
  drmModeRes *res;
  drmModeConnector *conn;
  unsigned int i;
  int ret;

  res = drmModeGetResources(fd);
  if (!res) {
    exit(1);
  }

  for (i = 0; i < res->count_connectors; ++i) {
    conn = drmModeGetConnector(fd, res->connectors[i]);
    if (!conn) {
      fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
	      i, res->connectors[i], errno);
      continue;
    }
    

    if (conn->connection != DRM_MODE_CONNECTED) {
      continue;
    }

    if(find_crtc(fd, res, conn) == false) {
      drmModeFreeConnector(conn);
      continue;
    }

    drmModeFreeConnector(conn);
  }

  drmModeFreeResources(res);

  
  return 0;
}



void page_flip_handler2(int fd,
			unsigned int sequence,
			unsigned int tv_sec,
			unsigned int tv_usec,
			unsigned int crtc_id,
			void *user_data) {

  for(auto&& conn : conn_list) {
    if (conn.crtc_id_ == crtc_id) {

      return;
    }
  }
  std::cout << "nothing to flip handler!" << std::endl;
}

auto create_buffer(int fd, uint32_t width, uint32_t height) {
  
  struct drm_mode_create_dumb creq;
  creq.width = width;
  creq.height = height;
  creq.bpp = 32;

  auto ret = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
  if (ret) {
    std::cerr << "create dumb failed" << std::endl;
    exit(1);
  }

  drm_mode_map_dumb mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.handle = creq.handle;
    
  ret = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

  auto map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
  if (map == MAP_FAILED) {
    std::cerr << "map failed" << std::endl;
    exit(1);
  }

  return std::make_tuple(creq.handle, map, creq.pitch);
}

bool init_egl_buffer() {
  const EGLint khr_image_attrs[] = {EGL_DMA_BUF_PLANE0_FD_EXT,
                                    buffer.buffer_handle_,
                                    EGL_WIDTH,
                                    (int)buffer.width_,
                                    EGL_HEIGHT,
                                    (int)buffer.height_,
                                    EGL_LINUX_DRM_FOURCC_EXT,
                                    GBM_FORMAT_XRGB8888,
                                    EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                    static_cast<const int>(buffer.stride_),
                                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                    static_cast<const int>(buffer.offset_),
                                    EGL_NONE};

  auto image =
      buffer.CreateImageKHR(buffer.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                          nullptr /* no client buffer */, khr_image_attrs);
  if (buffer.image_ == EGL_NO_IMAGE_KHR) {
    // fprintf(stderr, "failed to make image from buffer object: %s\n",
    //         EglGetError());
    return false;
  }

  glGenTextures(1, &buffer.gl_tex);
  glBindTexture(GL_TEXTURE_2D, buffer.gl_tex);
  buffer.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, buffer.image_);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &buffer.gl_fb);
  glBindFramebuffer(GL_FRAMEBUFFER, buffer.gl_fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buffer.gl_tex, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr,
            "failed framebuffer check for created target buffer: %x\n",
            glCheckFramebufferStatus(GL_FRAMEBUFFER));
    glDeleteFramebuffers(1, &buffer.gl_fb);
    glDeleteTextures(1, &buffer.gl_tex);
    return false;
  }

  return true;
}


int main() {
  auto fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  auto ret = drmSetMaster(fd);

  int width = 1920;
  int height = 1080;
  
  int buffer_handle;
  void* buffer_map;
  int buffer_stride;
  uint32_t fb_handle;
  
  std::tie(buffer_handle, buffer_map, buffer_stride) = create_buffer(fd, width, height);
  buffer.buffer_handle_ = buffer_handle;
  buffer.buffer_map_ = buffer_map;
  buffer.stride_ = buffer_stride;
  
  ret = drmModeAddFB(fd, width, height, 24, 32, buffer_stride, buffer_handle, &fb_handle);
  prepare(fd);
  init_egl(fd, conn_list[0]);
  init_egl_buffer();
  
  for (auto i=0 ; i<conn_list.size() ; ++i) {
    auto&& conn = conn_list[i];
    auto crtc = drmModeGetCrtc(fd, conn.crtc_id_);
    auto drm_conn = drmModeGetConnector(fd, conn.conn_id_);
    
    conn.width_ = crtc->mode.hdisplay;
    conn.height_ = crtc->mode.vdisplay;
    if (i == 0)
      ret = drmModeSetCrtc(fd, conn.crtc_id_, fb_handle, 0, 0, &conn.conn_id_, 1, &drm_conn->modes[0]);
    else
      ret = drmModeSetCrtc(fd, conn.crtc_id_, fb_handle, 100, 100, &conn.conn_id_, 1, &drm_conn->modes[0]);
    drmModeFreeConnector(drm_conn);
    drmModeFreeCrtc(crtc);
    
  }
  auto color = (char)0xff;

  while(true) {
    // color++;
    uint8_t r = rand() % 0xff;
    uint8_t g = rand() % 0xff;
    uint8_t b = rand() % 0xff;
    for(auto i=0 ; i<height; ++i) {
      for(auto j=0 ; j<width; ++j) {
        auto map = (char*)buffer_map;
        map[i*buffer_stride + j * 4 + 0] = r++;
        map[i*buffer_stride + j * 4 + 1] = g++;
        map[i*buffer_stride + j * 4 + 2] = b++;
        map[i*buffer_stride + j * 4 + 3] = r;
      }
    }
    
    // for(auto&& conn : conn_list) {
      // memset(conn.buffer_map_, color, conn.width_ * conn.height_ * 4);
      drmModeClip clip; 
      clip.x1 = 0;
      clip.x2 = (uint16_t)width;
      clip.y1 = 0;
      clip.y1 = (uint16_t)height;
      drmModeDirtyFB(fd, fb_handle, &clip, 1);
      sleep(1);
      // auto ret = drmModePageFlip(fd, conn.crtc_id_,
      // 				 conn.fb_handle_,
      // 				 DRM_MODE_PAGE_FLIP_EVENT, 0);
    
      // drmEventContext evctx;
      // fd_set rfds;

      // FD_ZERO(&rfds);
      // FD_SET(fd, &rfds);

      // while (select(fd + 1, &rfds, NULL, NULL, NULL) == -1);

      // memset(&evctx, 0, sizeof evctx);
      // evctx.version = DRM_EVENT_CONTEXT_VERSION;
      // evctx.page_flip_handler2 = page_flip_handler2;
    
      // drmHandleEvent(fd, &evctx);
    // }
    
  }

}
// int init_egl(const Connection& conn)
// {
//   EGLint major, minor;

//   auto dpy = eglGetDisplay((void*)c->gbm);
//   if (!dpy)
//     return -EINVAL;

//   eglInitialize(c->dpy, &major, &minor);
//   eglBindAPI(EGL_OPENGL_API);

//   c->ctx = eglCreateContext(c->dpy, NULL, EGL_NO_CONTEXT, NULL);
//   if (!c->ctx)
//     return -EINVAL;

//   eglMakeCurrent(c->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c->ctx);

//   glGenFramebuffers(1, &c->fb);
//   glBindFramebuffer(GL_FRAMEBUFFER_EXT, c->fb);

//   c->image = eglCreateImageKHR(c->dpy, NULL, EGL_NATIVE_PIXMAP_KHR,
// 			       c->bo, NULL);

//   glGenRenderbuffers(1, &c->color_rb);
//   glBindRenderbuffer(GL_RENDERBUFFER_EXT, c->color_rb);
//   glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER_EXT, c->image);
//   glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,
// 			       GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, c->color_rb);

//   glGenRenderbuffers(1, &c->depth_rb);
//   glBindRenderbuffer(GL_RENDERBUFFER_EXT, c->depth_rb);
//   glRenderbufferStorage(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT,
// 			c->mode->hdisplay, c->mode->vdisplay);
//   glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,
// 			       GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, c->depth_rb);

//   if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) !=
//       GL_FRAMEBUFFER_COMPLETE)
//     return -EINVAL;

//   return 0;
// }

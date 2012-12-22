/**
 * Copyright © 2011-2012 Kirill Gavrilov <kirill@sview.ru>
 *
 * Distributed under the Boost Software License, Version 1.0.
 * See accompanying file license-boost.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt
 */

#include <StImage/StLibAVImage.h>

#include <StAVPacket.h>
#include <StFile/StFileNode.h>
#include <StFile/StRawFile.h>
#include <StStrings/StLogger.h>

bool StLibAVImage::init() {
    return stLibAV::init();
}

StLibAVImage::StLibAVImage()
: StImageFile(),
  imageFormat(NULL),
  formatCtx(NULL),
  codecCtx(NULL),
  codec(NULL),
  frame(NULL) {
    StLibAVImage::init();
    imageFormat = av_find_input_format("image2");
    frame = avcodec_alloc_frame();
}

StLibAVImage::~StLibAVImage() {
    close();
    av_free(frame);
}

int StLibAVImage::getAVPixelFormat() {
    bool isFullScale = false;
    if(isPacked()) {
        switch(getPlane(0).getFormat()) {
            case StImagePlane::ImgRGB:  return stLibAV::PIX_FMT::RGB24;
            case StImagePlane::ImgBGR:  return stLibAV::PIX_FMT::BGR24;
            case StImagePlane::ImgRGBA: return stLibAV::PIX_FMT::RGBA32;
            case StImagePlane::ImgBGRA: return stLibAV::PIX_FMT::BGRA32;
            case StImagePlane::ImgGray: return stLibAV::PIX_FMT::GRAY8;
            default: return stLibAV::PIX_FMT::NONE;
        }
    }
    switch(getColorModel()) {
        case StImage::ImgColor_YUVjpeg:
            isFullScale = true;
        case StImage::ImgColor_YUV: {
            size_t aDelimX = (getPlane(1).getSizeX() > 0) ? (getPlane(0).getSizeX() / getPlane(1).getSizeX()) : 1;
            size_t aDelimY = (getPlane(1).getSizeY() > 0) ? (getPlane(0).getSizeY() / getPlane(1).getSizeY()) : 1;
            if(aDelimX == 1 && aDelimY == 1) {
                return isFullScale ? stLibAV::PIX_FMT::YUVJ444P : stLibAV::PIX_FMT::YUV444P;
            } else if(aDelimX == 2 && aDelimY == 2) {
                return isFullScale ? stLibAV::PIX_FMT::YUVJ420P : stLibAV::PIX_FMT::YUV420P;
            } else if(aDelimX == 2 && aDelimY == 1) {
                return isFullScale ? stLibAV::PIX_FMT::YUVJ422P : stLibAV::PIX_FMT::YUV422P;
            } else if(aDelimX == 1 && aDelimY == 2) {
                return isFullScale ? stLibAV::PIX_FMT::YUVJ440P : stLibAV::PIX_FMT::YUV440P;
            } else if(aDelimX == 4 && aDelimY == 1) {
                return stLibAV::PIX_FMT::YUV411P;
            } else if(aDelimX == 4 && aDelimY == 4) {
                return stLibAV::PIX_FMT::YUV410P;
            }
            return stLibAV::PIX_FMT::NONE;
        }
        default: return stLibAV::PIX_FMT::NONE;
    }
}

static void fillPointersAV(const StImage& theImage,
                           uint8_t* theData[], int theLinesize[]) {
    for(size_t aPlaneId = 0; aPlaneId < 4; ++aPlaneId) {
        theData[aPlaneId] = !theImage.getPlane(aPlaneId).isNull() ? (uint8_t* )theImage.getPlane(aPlaneId).getData() : NULL;
        theLinesize[aPlaneId] = (int )theImage.getPlane(aPlaneId).getSizeRowBytes();
    }
}

/**
 * Convert image from one format to another using swscale.
 * Image buffers should be already initialized!
 */
static bool convert(const StImage& theImageFrom, PixelFormat theFormatFrom,
                          StImage& theImageTo,   PixelFormat theFormatTo) {
    ST_DEBUG_LOG("StLibAVImage, convert from " + theFormatFrom + " to " + theFormatTo + " using swscale");
    SwsContext* pToRgbCtx = sws_getContext((int )theImageFrom.getSizeX(), (int )theImageFrom.getSizeY(), theFormatFrom, // source
                                           (int )theImageTo.getSizeX(),   (int )theImageTo.getSizeY(),   theFormatTo,   // destination
                                           SWS_BICUBIC, NULL, NULL, NULL);
    if(pToRgbCtx == NULL) {
        return false;
    }

    uint8_t* aSrcData[4]; int aSrcLinesize[4];
    fillPointersAV(theImageFrom, aSrcData, aSrcLinesize);

    uint8_t* aDstData[4]; int aDstLinesize[4];
    fillPointersAV(theImageTo, aDstData, aDstLinesize);

    sws_scale(pToRgbCtx,
              aSrcData, aSrcLinesize,
              0, (int )theImageFrom.getSizeY(),
              aDstData, aDstLinesize);

    sws_freeContext(pToRgbCtx);
    return true;
}

void StLibAVImage::close() {
    if(codec != NULL && codecCtx != NULL) {
        avcodec_close(codecCtx); // close VIDEO codec
        codec = NULL;
    }
    if(formatCtx != NULL) {
    #if(LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(53, 17, 0))
        avformat_close_input(&formatCtx);
    #else
        av_close_input_file(formatCtx); // close video file
        formatCtx = NULL;
    #endif
        // codec context allocated by av_open_input_file() function
        codecCtx = NULL;
    } else if(codecCtx != NULL) {
        // codec context allocated by ourself with avcodec_alloc_context() function
        av_free(codecCtx);
        codecCtx = NULL;
    }
}

bool StLibAVImage::load(const StString& theFilePath, ImageType theImageType,
                        uint8_t* theDataPtr, int theDataSize) {

    // reset current data
    StImage::nullify();
    setState();
    close();

    if(theImageType == ST_TYPE_NONE || !StFileNode::isFileExists(theFilePath)) {
        // open image file and detect its type, its could be non local file!
    #if(LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(53, 2, 0))
        int avErrCode = avformat_open_input(&formatCtx, theFilePath.toCString(), imageFormat, NULL);
    #else
        int avErrCode = av_open_input_file(&formatCtx, theFilePath.toCString(), imageFormat, 0, NULL);
    #endif
        if(avErrCode != 0) {
            setState(StString("AVFormat library, couldn't open image file. Error: ") + stLibAV::getAVErrorDescription(avErrCode));
            close();
            return false;
        }

        // dump information about file onto standard output (console)
        //dump_format(formatCtx, 0, strLoadVideo.c_str(), false);
        unsigned int streamId = 0;
        for(; streamId < formatCtx->nb_streams; ++streamId) {
            if(formatCtx->streams[streamId]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                break;
            }
        }

        if(streamId >= formatCtx->nb_streams) {
            setState("AVFormat library, couldn't find image stream");
            close();
            return false;
        }

        // find the decoder for the video stream
        codecCtx = formatCtx->streams[streamId]->codec;
    } else {
        // use given image type to load decoder
        codecCtx = avcodec_alloc_context();
    }

    // stupid check
    if(codecCtx == NULL) {
        setState("AVCodec library, codec context is NULL");
        close();
        return false;
    }

    switch(theImageType) {
        case ST_TYPE_PNG:
        case ST_TYPE_PNS: {
            codec = avcodec_find_decoder_by_name("png");
            break;
        }
        case ST_TYPE_JPEG:
        case ST_TYPE_MPO:
        case ST_TYPE_JPS: {
            codec = avcodec_find_decoder_by_name("mjpeg");
            break;
        }
        case ST_TYPE_NONE: {
            codec = avcodec_find_decoder(codecCtx->codec_id);
            break;
        }
        default: {
            setState(StString("StLibAVImage, unsupported image type id #") + theImageType + '!');
            close();
            return false;
        }
    }

    if(codec == NULL) {
        setState("AVCodec library, video codec not found");
        close();
        return false;
    }

    // open VIDEO codec
#if(LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53, 8, 0))
    if(avcodec_open2(codecCtx, codec, NULL) < 0) {
#else
    if(avcodec_open(codecCtx, codec) < 0) {
#endif
        setState("AVCodec library, could not open video codec");
        close();
        return false;
    }

    // read one packet or file
    StRawFile aRawFile(theFilePath);
    AVPacket avPacket;
    avPacket.destruct = NULL;
    if(theDataPtr == NULL || theDataSize == 0) {
        if(formatCtx != NULL) {
            avPacket.data = NULL;
            avPacket.size = 0;
            if(av_read_frame(formatCtx, &avPacket) < 0) {
                setState("AVFormat library, could not read first packet");
                close();
                return false;
            }
            theDataPtr  = avPacket.data;
            theDataSize = avPacket.size;
        } else {
            if(!aRawFile.readFile()) {
                setState("StLibAVImage, could not read the file");
                close();
                return false;
            }
            theDataPtr  = (uint8_t* )aRawFile.getBuffer();
            theDataSize = (int )aRawFile.getSize();
        }
    }

    // decode one frame
    int isFrameFinished = 0;

#if(LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(52, 23, 0))
    StAVPacket anAvPkt;
    anAvPkt.getAVpkt()->data = theDataPtr;
    anAvPkt.getAVpkt()->size = theDataSize;
    anAvPkt.setKeyFrame();
    avcodec_decode_video2(codecCtx, frame, &isFrameFinished, anAvPkt.getAVpkt());
#else
    avcodec_decode_video(codecCtx, frame, &isFrameFinished,
                         theDataPtr, theDataSize);
#endif

    if(isFrameFinished == 0) {
        // thats not an image!!! try to decode more packets???
        setState("AVCodec library, input file is not an Image!");
        close();
        return false;
    }

    // check frame size
    if(codecCtx->width <= 0 || codecCtx->height <= 0) {
        setState("AVCodec library, codec returns wrong frame size");
        close();
        return false;
    }

    size_t aWidthY, aHeightY, aWidthU, aHeightU, aWidthV, aHeightV;
    bool isFullScale = false;
    if(codecCtx->pix_fmt == stLibAV::PIX_FMT::RGB24) {
        setColorModel(StImage::ImgColor_RGB);
        changePlane(0).initWrapper(StImagePlane::ImgRGB, frame->data[0],
                                   codecCtx->width, codecCtx->height,
                                   frame->linesize[0]);
    } else if(codecCtx->pix_fmt == stLibAV::PIX_FMT::BGR24) {
        setColorModel(StImage::ImgColor_RGB);
        changePlane(0).initWrapper(StImagePlane::ImgBGR, frame->data[0],
                                   codecCtx->width, codecCtx->height,
                                   frame->linesize[0]);
    } else if(codecCtx->pix_fmt == stLibAV::PIX_FMT::RGBA32) {
        setColorModel(StImage::ImgColor_RGBA);
        changePlane(0).initWrapper(StImagePlane::ImgRGBA, frame->data[0],
                                   codecCtx->width, codecCtx->height,
                                   frame->linesize[0]);
    } else if(codecCtx->pix_fmt == stLibAV::PIX_FMT::BGRA32) {
        setColorModel(StImage::ImgColor_RGBA);
        changePlane(0).initWrapper(StImagePlane::ImgBGRA, frame->data[0],
                                   codecCtx->width, codecCtx->height,
                                   frame->linesize[0]);
    } else if(codecCtx->pix_fmt == stLibAV::PIX_FMT::GRAY8) {
        setColorModel(StImage::ImgColor_GRAY);
        changePlane(0).initWrapper(StImagePlane::ImgGray, frame->data[0],
                                   codecCtx->width, codecCtx->height,
                                   frame->linesize[0]);
    } else if(stLibAV::isFormatYUVPlanar(codecCtx,
                                          aWidthY, aHeightY,
                                          aWidthU, aHeightU,
                                          aWidthV, aHeightV,
                                          isFullScale)) {
        setColorModel(isFullScale ? StImage::ImgColor_YUVjpeg : StImage::ImgColor_YUV);
        changePlane(0).initWrapper(StImagePlane::ImgGray, frame->data[0],
                                   aWidthY, aHeightY, frame->linesize[0]);
        changePlane(1).initWrapper(StImagePlane::ImgGray, frame->data[1],
                                   aWidthU, aHeightU, frame->linesize[1]);
        changePlane(2).initWrapper(StImagePlane::ImgGray, frame->data[2],
                                   aWidthV, aHeightV, frame->linesize[2]);
    } else {
        ///ST_DEBUG_LOG("StLibAVImage, perform conversion from Pixel format '" + avcodec_get_pix_fmt_name(codecCtx->pix_fmt) + "' to RGB");
        // initialize software scaler/converter
        SwsContext* pToRgbCtx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt,       // source
                                               codecCtx->width, codecCtx->height, stLibAV::PIX_FMT::RGB24, // destination
                                               SWS_BICUBIC, NULL, NULL, NULL);
        if(pToRgbCtx == NULL) {
            setState("SWScale library, failed to create SWScaler context");
            close();
            return false;
        }

        // initialize additional buffer for converted RGB data
        setColorModel(StImage::ImgColor_RGB);
        changePlane(0).initTrash(StImagePlane::ImgRGB,
                                 codecCtx->width, codecCtx->height);

        uint8_t* rgbData[4]; stMemSet(rgbData,     0, sizeof(rgbData));
        int  rgbLinesize[4]; stMemSet(rgbLinesize, 0, sizeof(rgbLinesize));
        rgbData[0]     = changePlane(0).changeData();
        rgbLinesize[0] = (int )changePlane(0).getSizeRowBytes();

        sws_scale(pToRgbCtx,
                  frame->data, frame->linesize,
                  0, codecCtx->height,
                  rgbData, rgbLinesize);

        sws_freeContext(pToRgbCtx);
    }

    // destruct the package if needed
    if(avPacket.destruct != NULL) {
        avPacket.destruct(&avPacket);
    }

    // set debug information
    StString aDummy, aFileName;
    StFileNode::getFolderAndFile(theFilePath, aDummy, aFileName);
    setState(StString("AVCodec library, loaded image '") + aFileName + "' " + getDescription());

    // we should not close the file because decoded image data is in codec context cache
    return true;
}

bool StLibAVImage::save(const StString& theFilePath,
                         ImageType theImageType) {
    close();
    setState();
    if(isNull()) {
        return false;
    }

    PixelFormat aPFormatAV = (PixelFormat )getAVPixelFormat();
    StImage anImage;
    switch(theImageType) {
        case ST_TYPE_PNG:
        case ST_TYPE_PNS: {
            codec = avcodec_find_encoder_by_name("png");
            if(codec == NULL) {
                setState("AVCodec library, video codec 'png' not found");
                close();
                return false;
            }
            if(aPFormatAV == stLibAV::PIX_FMT::RGB24  ||
               aPFormatAV == stLibAV::PIX_FMT::RGBA32 ||
               aPFormatAV == stLibAV::PIX_FMT::GRAY8) {
                anImage.initWrapper(*this);
            } else {
                // convert to compatible pixel format
                anImage.changePlane().initTrash(StImagePlane::ImgRGB, getSizeX(), getSizeY(), getAligned(getSizeX() * 3));
                PixelFormat aPFrmtTarget = stLibAV::PIX_FMT::RGB24;
                if(!convert(*this,   aPFormatAV,
                            anImage, aPFrmtTarget)) {
                    setState("SWScale library, failed to create SWScaler context");
                    close();
                    return false;
                }
                aPFormatAV = aPFrmtTarget;
            }
            codecCtx = avcodec_alloc_context();

            // setup encoder
            codecCtx->pix_fmt = aPFormatAV;
            codecCtx->width  = (int )anImage.getSizeX();
            codecCtx->height = (int )anImage.getSizeY();
            codecCtx->compression_level = 9; // 0..9
            break;
        }
        case ST_TYPE_JPEG:
        case ST_TYPE_MPO:
        case ST_TYPE_JPS: {
            codec = avcodec_find_encoder_by_name("mjpeg");
            if(codec == NULL) {
                setState("AVCodec library, video codec 'mjpeg' not found");
                close();
                return false;
            }

            if(aPFormatAV == stLibAV::PIX_FMT::YUVJ420P
            || aPFormatAV == stLibAV::PIX_FMT::YUVJ422P
            //|| aPFormatAV == stLibAV::PIX_FMT::YUVJ444P not supported by FFmpeg... yet?
            //|| aPFormatAV == stLibAV::PIX_FMT::YUVJ440P
               ) {
                anImage.initWrapper(*this);
            } else {
                // convert to compatible pixel format
                PixelFormat aPFrmtTarget = stLibAV::PIX_FMT::YUVJ422P;
                anImage.setColorModel(StImage::ImgColor_YUVjpeg);
                anImage.changePlane(0).initTrash(StImagePlane::ImgGray, getSizeX(), getSizeY(), getAligned(getSizeX()));
                stMemSet(anImage.changePlane(0).changeData(), '\0', anImage.getPlane(0).getSizeBytes());
                anImage.changePlane(1).initTrash(StImagePlane::ImgGray, getSizeX(), getSizeY(), getAligned(getSizeX()));
                stMemSet(anImage.changePlane(1).changeData(), '\0', anImage.getPlane(1).getSizeBytes());
                anImage.changePlane(2).initTrash(StImagePlane::ImgGray, getSizeX(), getSizeY(), getAligned(getSizeX()));
                stMemSet(anImage.changePlane(2).changeData(), '\0', anImage.getPlane(2).getSizeBytes());
                if(!convert(*this,   aPFormatAV,
                            anImage, aPFrmtTarget)) {
                    setState("SWScale library, failed to create SWScaler context");
                    close();
                    return false;
                }
                aPFormatAV = aPFrmtTarget;
            }

            codecCtx = avcodec_alloc_context();
            codecCtx->pix_fmt = aPFormatAV;
            codecCtx->width  = (int )anImage.getSizeX();
            codecCtx->height = (int )anImage.getSizeY();
            codecCtx->time_base.num = 1;
            codecCtx->time_base.den = 1;
            codecCtx->qmin = codecCtx->qmax = 10; // quality factor - lesser is better
            break;
        }
        case ST_TYPE_NONE:
        default:
            close();
            return false;
    }

    // open VIDEO codec
#if(LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53, 8, 0))
    if(avcodec_open2(codecCtx, codec, NULL) < 0) {
#else
    if(avcodec_open(codecCtx, codec) < 0) {
#endif
        setState("AVCodec library, could not open video codec");
        close();
        return false;
    }

    // wrap own data into AVFrame
    fillPointersAV(anImage, frame->data, frame->linesize);

    StRawFile aRawFile(theFilePath);
    if(!aRawFile.openFile(StRawFile::WRITE)) {
        setState("Can not open the file for writing");
        close();
        return false;
    }

    // allocate the buffer, large enough (stupid formula copied from ffmpeg.c)
    int aBuffSize = int(getSizeX() * getSizeY() * 10);
    aRawFile.initBuffer(aBuffSize);

    // encode the image
    int anEncSize = avcodec_encode_video(codecCtx, (uint8_t* )aRawFile.changeBuffer(), aBuffSize, frame);
    if(anEncSize <= 0) {
        setState("AVCodec library, fail to encode the image");
        close();
        return false;
    }

    // store current content
    aRawFile.writeFile(anEncSize);
    // and finally close the file handle
    aRawFile.closeFile();

    close();

    // set debug information
    StString aDummy, aFileName;
    StFileNode::getFolderAndFile(theFilePath, aDummy, aFileName);
    setState(StString("AVCodec library, saved image '") + aFileName + "' " + getDescription());

    return true;
}

// Copyright 2015 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////////

#include "src/tiff_parser.h"

#include <cstring>

#include "src/tiff_directory/tiff_directory.h"

namespace piex {
namespace {

using tiff_directory::Endian;
using tiff_directory::Rational;
using tiff_directory::SRational;
using tiff_directory::SizeOfType;
using tiff_directory::TIFF_TYPE_LONG;
using tiff_directory::TIFF_TYPE_UNDEFINED;
using tiff_directory::TiffDirectory;
using tiff_directory::kBigEndian;
using tiff_directory::kLittleEndian;

// Specifies all tags that might be of interest to parse JPEG data.
const std::uint32_t kStartOfFrame = 0xFFC0;
const std::uint32_t kStartOfImage = 0xFFD8;
const std::uint32_t kStartOfScan = 0xFFDA;

// Reads the width and height of the full resolution image. The tag groups are
// exclusive.
bool GetFullDimension(const TiffDirectory& tiff_directory, std::uint32_t* width,
                      std::uint32_t* height) {
  if (tiff_directory.Has(kExifTagWidth) && tiff_directory.Has(kExifTagHeight)) {
    if (!tiff_directory.Get(kExifTagWidth, width) ||
        !tiff_directory.Get(kExifTagHeight, height)) {
      return false;
    }
  } else if (tiff_directory.Has(kTiffTagImageWidth) &&
             tiff_directory.Has(kTiffTagImageLength)) {
    if (!tiff_directory.Get(kTiffTagImageWidth, width) ||
        !tiff_directory.Get(kTiffTagImageLength, height)) {
      return false;
    }
  } else if (tiff_directory.Has(kPanaTagTopBorder) &&
             tiff_directory.Has(kPanaTagLeftBorder) &&
             tiff_directory.Has(kPanaTagBottomBorder) &&
             tiff_directory.Has(kPanaTagRightBorder)) {
    std::uint32_t left;
    std::uint32_t right;
    std::uint32_t top;
    std::uint32_t bottom;
    if (tiff_directory.Get(kPanaTagLeftBorder, &left) &&
        tiff_directory.Get(kPanaTagRightBorder, &right) &&
        tiff_directory.Get(kPanaTagTopBorder, &top) &&
        tiff_directory.Get(kPanaTagBottomBorder, &bottom) && bottom > top &&
        right > left) {
      *height = bottom - top;
      *width = right - left;
    } else {
      return false;
    }
  } else if (tiff_directory.Has(kExifTagDefaultCropSize)) {
    std::vector<std::uint32_t> crop(2);
    std::vector<Rational> crop_rational(2);
    if (tiff_directory.Get(kExifTagDefaultCropSize, &crop)) {
      *width = crop[0];
      *height = crop[1];
    } else if (tiff_directory.Get(kExifTagDefaultCropSize, &crop_rational) &&
               crop_rational[0].denominator != 0 &&
               crop_rational[1].denominator != 0) {
      *width = crop_rational[0].numerator / crop_rational[0].denominator;
      *height = crop_rational[1].numerator / crop_rational[1].denominator;
    } else {
      return false;
    }
  }
  return true;
}

bool GetRational(const TiffDirectory::Tag& tag, const TiffDirectory& directory,
                 const int data_size, PreviewImageData::Rational* data) {
  std::vector<Rational> value;
  if (directory.Get(tag, &value) &&
      value.size() == static_cast<size_t>(data_size)) {
    for (size_t i = 0; i < value.size(); ++i) {
      data[i].numerator = value[i].numerator;
      data[i].denominator = value[i].denominator;
    }
    return true;
  }
  return false;
}

void FillGpsPreviewImageData(const TiffDirectory& gps_directory,
                             PreviewImageData* preview_image_data) {
  if (gps_directory.Has(kGpsTagLatitudeRef) &&
      gps_directory.Has(kGpsTagLatitude) &&
      gps_directory.Has(kGpsTagLongitudeRef) &&
      gps_directory.Has(kGpsTagLongitude) &&
      gps_directory.Has(kGpsTagTimeStamp) &&
      gps_directory.Has(kGpsTagDateStamp)) {
    preview_image_data->gps.is_valid = false;
    std::string value;
    if (!gps_directory.Get(kGpsTagLatitudeRef, &value) || value.empty() ||
        (value[0] != 'N' && value[0] != 'S') ||
        !GetRational(kGpsTagLatitude, gps_directory, 3 /* data size */,
                     preview_image_data->gps.latitude)) {
      return;
    }
    preview_image_data->gps.latitude_ref = value[0];

    if (!gps_directory.Get(kGpsTagLongitudeRef, &value) || value.empty() ||
        (value[0] != 'E' && value[0] != 'W') ||
        !GetRational(kGpsTagLongitude, gps_directory, 3 /* data size */,
                     preview_image_data->gps.longitude)) {
      return;
    }
    preview_image_data->gps.longitude_ref = value[0];

    if (!GetRational(kGpsTagTimeStamp, gps_directory, 3 /* data size */,
                     preview_image_data->gps.time_stamp)) {
      return;
    }

    const size_t kGpsDateStampSize = 11;
    if (!gps_directory.Get(kGpsTagDateStamp,
                           &preview_image_data->gps.date_stamp)) {
      return;
    }
    if (preview_image_data->gps.date_stamp.size() == kGpsDateStampSize) {
      // Resize the date_stamp to remove the "NULL" at the end of string.
      preview_image_data->gps.date_stamp.resize(kGpsDateStampSize - 1);
    } else {
      return;
    }

    if (gps_directory.Has(kGpsTagAltitudeRef) &&
        gps_directory.Has(kGpsTagAltitude)) {
      std::vector<std::uint8_t> bytes;
      if (!gps_directory.Get(kGpsTagAltitudeRef, &bytes) || bytes.empty() ||
          !GetRational(kGpsTagAltitude, gps_directory, 1,
                       &preview_image_data->gps.altitude)) {
        return;
      }
      preview_image_data->gps.altitude_ref = bytes[0] != 0;
    }
    preview_image_data->gps.is_valid = true;
  }
}

Error FillPreviewImageData(const TiffDirectory& tiff_directory,
                           PreviewImageData* preview_image_data) {
  bool success = true;
  // Get preview_offset and preview_length
  if (tiff_directory.Has(kTiffTagStripOffsets) &&
      tiff_directory.Has(kTiffTagStripByteCounts)) {
    std::vector<std::uint32_t> strip_offsets;
    std::vector<std::uint32_t> strip_byte_counts;
    if (!tiff_directory.Get(kTiffTagStripOffsets, &strip_offsets) ||
        !tiff_directory.Get(kTiffTagStripByteCounts, &strip_byte_counts)) {
      return kFail;
    }
    if (strip_offsets.size() == 1 && strip_byte_counts.size() == 1) {
      preview_image_data->preview.offset = strip_offsets[0];
      preview_image_data->preview.length = strip_byte_counts[0];
      // TODO: remove old vars.
      preview_image_data->preview_offset = strip_offsets[0];
      preview_image_data->preview_length = strip_byte_counts[0];
    }
  } else if (tiff_directory.Has(kTiffTagJpegOffset) &&
             tiff_directory.Has(kTiffTagJpegByteCount)) {
    success &= tiff_directory.Get(kTiffTagJpegOffset,
                                  &preview_image_data->preview.offset);
    success &= tiff_directory.Get(kTiffTagJpegByteCount,
                                  &preview_image_data->preview.length);
    // TODO: remove old vars.
    preview_image_data->preview_offset = preview_image_data->preview.offset;
    preview_image_data->preview_length = preview_image_data->preview.length;

  } else if (tiff_directory.Has(kPanaTagJpegImage)) {
    if (!tiff_directory.GetOffsetAndLength(
            kPanaTagJpegImage, TIFF_TYPE_UNDEFINED,
            &preview_image_data->preview.offset,
            &preview_image_data->preview.length)) {
      return kFail;
    }
    // TODO: remove old vars.
    preview_image_data->preview_offset = preview_image_data->preview.offset;
    preview_image_data->preview_length = preview_image_data->preview.length;
  }

  // Get exif_orientation if it was not set already.
  if (tiff_directory.Has(kTiffTagOrientation) &&
      preview_image_data->exif_orientation == 1) {
    success &= tiff_directory.Get(kTiffTagOrientation,
                                  &preview_image_data->exif_orientation);
  }

  // Get color_space
  if (tiff_directory.Has(kExifTagColorSpace)) {
    std::uint32_t color_space;
    success &= tiff_directory.Get(kExifTagColorSpace, &color_space);
    if (color_space == 1) {
      preview_image_data->color_space = PreviewImageData::kSrgb;
    } else if (color_space == 65535 || color_space == 2) {
      preview_image_data->color_space = PreviewImageData::kAdobeRgb;
    }
  }

  success &= GetFullDimension(tiff_directory, &preview_image_data->full_width,
                              &preview_image_data->full_height);

  if (tiff_directory.Has(kTiffTagMake)) {
    success &= tiff_directory.Get(kTiffTagMake, &preview_image_data->maker);
  }

  if (tiff_directory.Has(kTiffTagModel)) {
    success &= tiff_directory.Get(kTiffTagModel, &preview_image_data->model);
  }

  if (tiff_directory.Has(kTiffTagCfaPatternDim)) {
    std::vector<std::uint32_t> cfa_pattern_dim;
    if (tiff_directory.Get(kTiffTagCfaPatternDim, &cfa_pattern_dim) &&
        cfa_pattern_dim.size() == 2) {
      preview_image_data->cfa_pattern_dim[0] = cfa_pattern_dim[0];
      preview_image_data->cfa_pattern_dim[1] = cfa_pattern_dim[1];
    }
  }

  if (tiff_directory.Has(kExifTagDateTimeOriginal)) {
    success &= tiff_directory.Get(kExifTagDateTimeOriginal,
                                  &preview_image_data->date_time);
  }

  if (tiff_directory.Has(kExifTagIsoSpeed)) {
    success &= tiff_directory.Get(kExifTagIsoSpeed, &preview_image_data->iso);
  } else if (tiff_directory.Has(kPanaTagIso)) {
    success &= tiff_directory.Get(kPanaTagIso, &preview_image_data->iso);
  }

  if (tiff_directory.Has(kExifTagExposureTime)) {
    success &= GetRational(kExifTagExposureTime, tiff_directory, 1,
                           &preview_image_data->exposure_time);
  }

  if (tiff_directory.Has(kExifTagFnumber)) {
    success &= GetRational(kExifTagFnumber, tiff_directory, 1,
                           &preview_image_data->fnumber);
  }

  if (tiff_directory.Has(kExifTagFocalLength)) {
    success &= GetRational(kExifTagFocalLength, tiff_directory, 1,
                           &preview_image_data->focal_length);
  }

  if (!success) {
    return kFail;
  }

  return kOk;
}

const TiffDirectory* FindFirstTagInIfds(const TiffDirectory::Tag& tag,
                                        const IfdVector& tiff_directory) {
  for (std::uint32_t i = 0; i < tiff_directory.size(); ++i) {
    if (tiff_directory[i].Has(tag)) {
      return &tiff_directory[i];
    }

    // Recursively search sub directories.
    const TiffDirectory* sub_directory =
        FindFirstTagInIfds(tag, tiff_directory[i].GetSubDirectories());
    if (sub_directory != NULL) {
      return sub_directory;
    }
  }
  return NULL;
}

// Gets the SubIfd content.
void ParseSubIfds(const std::uint32_t tiff_offset, const TagSet& desired_tags,
                  const std::uint32_t max_number_ifds, const Endian endian,
                  StreamInterface* stream, TiffDirectory* tiff_ifd,
                  Error* error) {
  if (*error == kOk && tiff_ifd->Has(kTiffTagSubIfd)) {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    tiff_ifd->GetOffsetAndLength(kTiffTagSubIfd, TIFF_TYPE_LONG, &offset,
                                 &length);
    length /= 4;  // length in bytes divided by 4 gives number of IFDs.
    for (std::uint32_t j = 0; j < length && j < max_number_ifds; ++j) {
      std::uint32_t sub_offset;
      if (!Get32u(stream, offset + 4 * j, endian, &sub_offset)) {
        *error = kFail;
        return;
      }

      std::uint32_t next_ifd_offset;
      TiffDirectory sub_ifd(static_cast<Endian>(endian));
      *error = ParseDirectory(tiff_offset, sub_offset, endian, desired_tags,
                              stream, &sub_ifd, &next_ifd_offset);
      if (*error != kOk) {
        return;
      }

      tiff_ifd->AddSubDirectory(sub_ifd);
    }
  }
}

}  // namespace

bool Get16u(StreamInterface* stream, const std::uint32_t offset,
            const Endian& endian, std::uint16_t* value) {
  std::uint8_t data[2];
  if (stream->GetData(offset, 2, data) == kOk) {
    if (endian == kBigEndian) {
      *value = (data[0] * 0x100) | data[1];
    } else {
      *value = (data[1] * 0x100) | data[0];
    }
    return true;
  } else {
    return false;
  }
}

bool Get32u(StreamInterface* stream, const std::uint32_t offset,
            const Endian& endian, std::uint32_t* value) {
  std::uint8_t data[4];
  if (stream->GetData(offset, 4, data) == kOk) {
    if (endian == kBigEndian) {
      *value = (data[0] * 0x1000000u) | (data[1] * 0x10000u) |
               (data[2] * 0x100u) | data[3];
    } else {
      *value = (data[3] * 0x1000000u) | (data[2] * 0x10000u) |
               (data[1] * 0x100u) | data[0];
    }
    return true;
  } else {
    return false;
  }
}

std::vector<std::uint8_t> GetData(const size_t offset, const size_t length,
                                  StreamInterface* stream, Error* error) {
  // Read in chunks with a maximum size of 1 MiB.
  const size_t kChunkSize = 1048576;

  std::vector<std::uint8_t> data;
  size_t processed_data = 0;
  while (*error == kOk && processed_data < length) {
    size_t chunk_length = kChunkSize;
    if (length - data.size() < kChunkSize) {
      chunk_length = length - data.size();
    }

    data.resize(processed_data + chunk_length);
    *error = stream->GetData(offset + processed_data, chunk_length,
                             &data[processed_data]);

    processed_data += chunk_length;
  }
  return data;
}

bool GetEndianness(const std::uint32_t tiff_offset, StreamInterface* stream,
                   Endian* endian) {
  const std::uint8_t kTiffBigEndianMagic[] = {'M', 'M'};
  const std::uint8_t kTiffLittleEndianMagic[] = {'I', 'I'};
  std::uint8_t tiff_endian[sizeof(kTiffBigEndianMagic)];
  if (stream->GetData(tiff_offset, sizeof(tiff_endian), &tiff_endian[0]) !=
      kOk) {
    return false;
  }

  if (!memcmp(tiff_endian, kTiffLittleEndianMagic, sizeof(tiff_endian))) {
    *endian = kLittleEndian;
    return true;
  } else if (!memcmp(tiff_endian, kTiffBigEndianMagic, sizeof(tiff_endian))) {
    *endian = kBigEndian;
    return true;
  } else {
    return false;
  }
}

bool GetPreviewDimensions(const std::uint32_t jpeg_offset,
                          StreamInterface* stream, std::uint16_t* width,
                          std::uint16_t* height) {
  const Endian endian = kBigEndian;
  std::uint32_t offset = jpeg_offset;
  std::uint16_t segment;

  // Parse the JPEG header until we find Frame0 which contains the image width
  // and height or the actual image data starts (StartOfScan)
  do {
    if (!Get16u(stream, offset, endian, &segment)) {
      return false;
    }
    offset += 2;

    switch (segment) {
      case kStartOfImage:
        break;
      case kStartOfFrame:
        return Get16u(stream, offset + 3, endian, height) &&
               Get16u(stream, offset + 5, endian, width);
      default: {
        std::uint16_t length;
        if (!Get16u(stream, offset, endian, &length)) {
          return false;
        }
        offset += length;
      }
    }
  } while (segment != kStartOfScan);

  // No width and hight information found.
  return false;
}

Error ParseDirectory(const std::uint32_t tiff_offset,
                     const std::uint32_t ifd_offset, const Endian endian,
                     const TagSet& desired_tags, StreamInterface* stream,
                     TiffDirectory* tiff_directory,
                     std::uint32_t* next_ifd_offset) {
  std::uint16_t number_of_entries;
  if (!Get16u(stream, ifd_offset, endian, &number_of_entries)) {
    return kFail;
  }

  for (std::uint32_t i = 0;
       i < static_cast<std::uint32_t>(number_of_entries) * 12; i += 12) {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint32_t number_of_elements;
    if (Get16u(stream, ifd_offset + 2 + i, endian, &tag) &&
        Get16u(stream, ifd_offset + 4 + i, endian, &type) &&
        Get32u(stream, ifd_offset + 6 + i, endian, &number_of_elements)) {
      // Check if the current tag should be handled.
      if (desired_tags.count(static_cast<TiffDirectory::Tag>(tag)) != 1) {
        continue;
      }
    } else {
      return kFail;
    }

    const size_t type_size = SizeOfType(type, nullptr /* no error */);

    // Check that type_size * number_of_elements does not exceed UINT32_MAX.
    if (type_size != 0 && number_of_elements > UINT32_MAX / type_size) {
      return kFail;
    }
    const size_t byte_count =
        type_size * static_cast<size_t>(number_of_elements);

    std::uint32_t value_offset;
    if (byte_count > 4 &&
        Get32u(stream, ifd_offset + 10 + i, endian, &value_offset)) {
      value_offset += tiff_offset;
    } else if (byte_count != 0) {
      value_offset = ifd_offset + 10 + i;
    } else {
      // Ignore entries with an invalid byte count.
      continue;
    }

    Error error = kOk;
    const std::vector<std::uint8_t> data =
        GetData(value_offset, byte_count, stream, &error);
    if (error != kOk) {
      return error;
    }
    tiff_directory->AddEntry(tag, type, number_of_elements, value_offset, data);
  }

  if (Get32u(stream, ifd_offset + 2u + number_of_entries * 12u, endian,
             next_ifd_offset)) {
    return kOk;
  } else {
    return kFail;
  }
}

TiffParser::TiffParser(StreamInterface* stream) : stream_(stream) {}

TiffParser::TiffParser(StreamInterface* stream, const std::uint32_t offset)
    : stream_(stream), tiff_offset_(offset) {}

Error TiffParser::GetPreviewImageData(const TiffContent& tiff_content,
                                      PreviewImageData* preview_image_data) {
  Error error = kOk;
  for (const auto& tiff_directory : tiff_content.tiff_directory) {
    error = FillPreviewImageData(tiff_directory, preview_image_data);
    if (error == kOk && tiff_directory.Has(kTiffTagExifIfd) &&
        tiff_content.exif_directory) {
      error = FillPreviewImageData(*tiff_content.exif_directory,
                                   preview_image_data);
    }
    if (error == kOk && tiff_directory.Has(kExifTagGps) &&
        tiff_content.gps_directory) {
      FillGpsPreviewImageData(*tiff_content.gps_directory, preview_image_data);
    }
    for (const auto& sub_directory : tiff_directory.GetSubDirectories()) {
      if (error == kOk) {
        error = FillPreviewImageData(sub_directory, preview_image_data);
      }
    }
  }
  return error;
}

Error TiffParser::Parse(const TagSet& desired_tags,
                        const std::uint16_t max_number_ifds,
                        TiffContent* tiff_content) {
  if (!tiff_content->tiff_directory.empty()) {
    return kFail;  // You shall call Parse() only once.
  }

  const std::uint32_t kTiffIdentifierSize = 4;
  std::uint32_t offset_to_ifd = 0;
  if (!GetEndianness(tiff_offset_, stream_, &endian_) ||
      !Get32u(stream_, tiff_offset_ + kTiffIdentifierSize, endian_,
              &offset_to_ifd)) {
    return kFail;
  }

  Error error = ParseIfd(tiff_offset_ + offset_to_ifd, desired_tags,
                         max_number_ifds, &tiff_content->tiff_directory);
  if (error != kOk) {
    return error;
  }

  // Get the Exif data.
  if (FindFirstTagInIfds(kTiffTagExifIfd, tiff_content->tiff_directory) !=
      nullptr) {
    const TiffDirectory* tiff_ifd =
        FindFirstTagInIfds(kTiffTagExifIfd, tiff_content->tiff_directory);
    std::uint32_t offset;
    if (tiff_ifd->Get(kTiffTagExifIfd, &offset)) {
      tiff_content->exif_directory.reset(new TiffDirectory(endian_));
      std::uint32_t next_ifd_offset;
      error = ParseDirectory(
          tiff_offset_, tiff_offset_ + offset, endian_, desired_tags, stream_,
          tiff_content->exif_directory.get(), &next_ifd_offset);
      if (error != kOk) {
        return error;
      }

      return ParseGpsData(tiff_ifd, tiff_content);
    }
  }

  // Get the GPS data from the tiff ifd.
  if (FindFirstTagInIfds(kExifTagGps, tiff_content->tiff_directory) !=
      nullptr) {
    const TiffDirectory* tiff_ifd =
        FindFirstTagInIfds(kExifTagGps, tiff_content->tiff_directory);
    return ParseGpsData(tiff_ifd, tiff_content);
  }

  return error;
}

Error TiffParser::ParseIfd(const std::uint32_t offset_to_ifd,
                           const TagSet& desired_tags,
                           const std::uint16_t max_number_ifds,
                           IfdVector* tiff_directory) {
  std::uint32_t next_ifd_offset;
  TiffDirectory tiff_ifd(static_cast<Endian>(endian_));
  Error error =
      ParseDirectory(tiff_offset_, offset_to_ifd, endian_, desired_tags,
                     stream_, &tiff_ifd, &next_ifd_offset);

  ParseSubIfds(tiff_offset_, desired_tags, max_number_ifds, endian_, stream_,
               &tiff_ifd, &error);
  if (error == kOk) {
    tiff_directory->push_back(tiff_ifd);
    if (next_ifd_offset != 0 && tiff_directory->size() < max_number_ifds) {
      error = ParseIfd(tiff_offset_ + next_ifd_offset, desired_tags,
                       max_number_ifds, tiff_directory);
    }
  }

  return error;
}

Error TiffParser::ParseGpsData(const TiffDirectory* tiff_ifd,
                               TiffContent* tiff_content) {
  std::uint32_t offset;
  if (tiff_ifd->Get(kExifTagGps, &offset)) {
    tiff_content->gps_directory.reset(new TiffDirectory(endian_));
    const TagSet gps_tags = {kGpsTagLatitudeRef,  kGpsTagLatitude,
                             kGpsTagLongitudeRef, kGpsTagLongitude,
                             kGpsTagAltitudeRef,  kGpsTagAltitude,
                             kGpsTagTimeStamp,    kGpsTagDateStamp};
    std::uint32_t next_ifd_offset;
    return ParseDirectory(tiff_offset_, tiff_offset_ + offset, endian_,
                          gps_tags, stream_, tiff_content->gps_directory.get(),
                          &next_ifd_offset);
  }
  return kOk;
}

}  // namespace piex

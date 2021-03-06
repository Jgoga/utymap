#include "index/BitmapStream.hpp"
#include "index/ElementStream.hpp"
#include "index/BitmapIndex.hpp"
#include "index/ElementGeometryVisitor.hpp"
#include "index/ElementVisitorFilter.hpp"
#include "index/PersistentElementStore.hpp"
#include "utils/LruCache.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <tuple>

using namespace utymap;
using namespace utymap::index;
using namespace utymap::entities;
using namespace utymap::mapcss;
using namespace utymap::utils;

namespace {
const std::string IndexFileExtension = ".idf";
const std::string DataFileExtension = ".dat";
const std::string bitmapFileExtension = ".bmp";

struct BitmapData {
  const std::string path;
  BitmapIndex::Bitmap data;

  BitmapData(const std::string &bitmapPath) :
    path(bitmapPath) {}

  BitmapData(BitmapData &&other) :
    path(std::move(other.path)),
    data(std::move(other.data)) {}
};

/// Stores file handlers related to data of specific quad key.
struct QuadKeyData {
  std::unique_ptr<std::fstream> dataFile;
  std::unique_ptr<std::fstream> indexFile;

  QuadKeyData(const std::string &dataPath,
              const std::string &indexPath,
              const std::string &bitmapPath) :
      dataFile(utymap::utils::make_unique<std::fstream>()),
      indexFile(utymap::utils::make_unique<std::fstream>()),
      dataPath_(dataPath),
      indexPath_(indexPath),
      bitmapPath_(bitmapPath),
      bitmapData_(utymap::utils::make_unique<BitmapData>(bitmapPath)) {
    using std::ios;
    dataFile->open(dataPath, ios::in | ios::out | ios::binary | ios::app | ios::ate);
    indexFile->open(indexPath, ios::in | ios::out | ios::binary | ios::app | ios::ate);
  }

  BitmapData& getBitmap() const {
    if (bitmapData_->data.empty()) {
      // TODO not thread safe!
      std::fstream bitmapFile;
      bitmapFile.open(bitmapData_->path, std::ios::in | std::ios::binary);
      BitmapStream::read(bitmapFile, bitmapData_->data);
    }
    return *bitmapData_;
  }

  QuadKeyData(const QuadKeyData &) = delete;
  QuadKeyData &operator=(const QuadKeyData &) = delete;

  QuadKeyData(QuadKeyData &&other) :
      dataFile(std::move(other.dataFile)),
      indexFile(std::move(other.indexFile)),
      dataPath_(std::move(other.dataPath_)),
      indexPath_(std::move(other.indexPath_)),
      bitmapPath_(std::move(other.bitmapPath_)),
      bitmapData_(std::move(other.bitmapData_)) {}

  ~QuadKeyData() {
    closeAll();
  }

  void erase() {
    closeAll();
    if (std::remove(dataPath_.c_str())) logEraseError(dataPath_);
    if (std::remove(indexPath_.c_str())) logEraseError(indexPath_);
    if (std::remove(bitmapPath_.c_str())) logEraseError(bitmapPath_);
  }

private:
  void closeAll() const {
    if (dataFile != nullptr && dataFile->good()) dataFile->close();
    if (indexFile != nullptr && indexFile->good()) indexFile->close();
  }

  static void logEraseError(const std::string &path) {
    std::cerr << "Cannot erase " << path << std::endl;
  }

  const std::string dataPath_;
  const std::string indexPath_;
  const std::string bitmapPath_;
  std::unique_ptr<BitmapData> bitmapData_;
};
}

using Cache = utymap::utils::LruCache<QuadKey, QuadKeyData, QuadKey::Comparator>;

// TODO improve thread safety!
class PersistentElementStore::PersistentElementStoreImpl : BitmapIndex {
 public:
  PersistentElementStoreImpl(const std::string &dataPath,
                             const StringTable &stringTable):
    BitmapIndex(stringTable),
    dataPath_(dataPath),
    lock_(),
    cache_(12) {}

  void store(const Element &element, const QuadKey &quadKey) {
    const auto &quadKeyData = getQuadKeyData(quadKey);
    auto offset = static_cast<std::uint32_t>(quadKeyData->dataFile->tellg());
    auto fileSize = quadKeyData->indexFile->tellg();
    auto order = static_cast<std::uint32_t>(fileSize / (sizeof(element.id) + sizeof(offset)));

    // write element index
    quadKeyData->indexFile->seekg(0, std::ios::end);
    quadKeyData->indexFile->write(reinterpret_cast<const char *>(&element.id), sizeof(element.id));
    quadKeyData->indexFile->write(reinterpret_cast<const char *>(&offset), sizeof(offset));

    // write element data
    quadKeyData->dataFile->seekg(0, std::ios::end);
    ElementStream::write(*quadKeyData->dataFile, element);

    // write element search data
    add(element, quadKey, order);
    // TODO we always clean/write the whole file here.
    std::fstream bitmapFile(quadKeyData->getBitmap().path, std::ios::out | std::ios::binary | std::ios::trunc);
    BitmapStream::write(bitmapFile, quadKeyData->getBitmap().data);
  }

  void search(const BitmapIndex::Query &query,
              ElementVisitor &visitor,
              const utymap::CancellationToken &cancelToken) {
    ElementVisitorFilter filter(visitor, [&](const Element &element) {
      return ElementGeometryVisitor::intersects(element, query.boundingBox);
    });
    BitmapIndex::search(query, filter);
  }

  void search(const QuadKey &quadKey,
              ElementVisitor &visitor,
              const utymap::CancellationToken &cancelToken) {
    const auto &quadKeyData = getQuadKeyData(quadKey);
    auto count = static_cast<std::uint32_t>(quadKeyData->indexFile->tellg() /
        (sizeof(std::uint64_t) + sizeof(std::uint32_t)));

    quadKeyData->indexFile->seekg(0, std::ios::beg);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (cancelToken.isCancelled()) break;

      auto entry = readIndexEntry(*quadKeyData);
      quadKeyData->dataFile->seekg(std::get<1>(entry), std::ios::beg);

      ElementStream::read(*quadKeyData->dataFile, std::get<0>(entry ))->accept(visitor);
    }
  }

  bool hasData(const QuadKey &quadKey) const override {
    std::ifstream file(getFilePath(quadKey, DataFileExtension));
    return file.good();
  }

  void erase(const utymap::QuadKey &quadKey) override {
    auto quadKeyData = getQuadKeyData(quadKey);
    {
      std::lock_guard<std::mutex> lock(lock_);
      quadKeyData->erase();
      cache_.clear();
    }
  }

  void erase(const utymap::BoundingBox &bbox, const utymap::LodRange &range) {
    throw std::domain_error("Deletion by bounding box and lod range is not implemented.");
  }

  void flush() {
    cache_.clear();
  }

 protected:
  void notify(const utymap::QuadKey& quadKey,
              const std::uint32_t order,
              ElementVisitor &visitor) override {
    auto quadKeyData = getQuadKeyData(quadKey);
    auto offset = order * (sizeof(std::uint64_t) + sizeof(std::uint32_t));

    quadKeyData->indexFile->seekg(offset, std::ios::beg);
    auto entry = readIndexEntry(*quadKeyData);
    quadKeyData->dataFile->seekg(std::get<1>(entry), std::ios::beg);

    ElementStream::read(*quadKeyData->dataFile, std::get<0>(entry))->accept(visitor);
  }

  Bitmap& getBitmap(const utymap::QuadKey& quadKey) override {
    return getQuadKeyData(quadKey)->getBitmap().data;
  }

 private:
  /// Gets quad key data.
  std::shared_ptr<QuadKeyData> getQuadKeyData(const QuadKey& quadKey) {
    std::lock_guard<std::mutex> lock(lock_);

    if (cache_.exists(quadKey))
      return cache_.get(quadKey);

    cache_.put(quadKey, std::move(QuadKeyData(getFilePath(quadKey, DataFileExtension),
                                              getFilePath(quadKey, IndexFileExtension),
                                              getFilePath(quadKey, bitmapFileExtension))));

    return cache_.get(quadKey);
  }

  /// Gets full file path for given quad key
  std::string getFilePath(const QuadKey &quadKey, const std::string &extension) const {
    std::stringstream ss;
    ss << dataPath_ << "/" << quadKey.levelOfDetail << "/" <<
      GeoUtils::quadKeyToString(quadKey) << extension;
    return ss.str();
  }

  /// Reads element info from index.
  std::tuple<std::uint64_t, std::uint32_t> readIndexEntry(const QuadKeyData &quadKeyData) {
    std::uint64_t id;
    std::uint32_t offset;
    quadKeyData.indexFile->read(reinterpret_cast<char *>(&id), sizeof(id));
    quadKeyData.indexFile->read(reinterpret_cast<char *>(&offset), sizeof(offset));
    return std::make_tuple(id, offset);
  }

  const std::string dataPath_;
  std::mutex lock_;
  utymap::utils::LruCache<QuadKey, QuadKeyData, QuadKey::Comparator> cache_;
};

PersistentElementStore::PersistentElementStore(const std::string &dataPath,
                                               const StringTable &stringTable) :
  ElementStore(stringTable),
  pimpl_(utymap::utils::make_unique<PersistentElementStoreImpl>(dataPath, stringTable)) {}

PersistentElementStore::~PersistentElementStore() {
}

void PersistentElementStore::save(const Element &element, const QuadKey &quadKey) {
  pimpl_->store(element, quadKey);
}

void PersistentElementStore::search(const std::string &notTerms,
                                    const std::string &andTerms,
                                    const std::string &orTerms,
                                    const utymap::BoundingBox &bbox,
                                    const utymap::LodRange &range,
                                    utymap::entities::ElementVisitor &visitor,
                                    const utymap::CancellationToken &cancelToken) {
  BitmapIndex::Query query = { notTerms, andTerms, orTerms, bbox, range };
  pimpl_->search(query, visitor, cancelToken);
}

void PersistentElementStore::search(const QuadKey &quadKey,
                                    ElementVisitor &visitor,
                                    const utymap::CancellationToken &cancelToken) {
  pimpl_->search(quadKey, visitor, cancelToken);
}

bool PersistentElementStore::hasData(const QuadKey &quadKey) const {
  return pimpl_->hasData(quadKey);
}

void PersistentElementStore::flush() {
  pimpl_->flush();
}

void PersistentElementStore::erase(const utymap::QuadKey &quadKey) {
  pimpl_->erase(quadKey);
}

void PersistentElementStore::erase(const utymap::BoundingBox &bbox,
                                   const utymap::LodRange &range) {
  pimpl_->erase(bbox, range);
}
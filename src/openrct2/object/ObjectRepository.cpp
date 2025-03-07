/*****************************************************************************
 * Copyright (c) 2014-2020 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ObjectRepository.h"

#include "../Context.h"
#include "../PlatformEnvironment.h"
#include "../common.h"
#include "../config/Config.h"
#include "../core/Console.hpp"
#include "../core/FileIndex.hpp"
#include "../core/FileStream.h"
#include "../core/Guard.hpp"
#include "../core/IStream.hpp"
#include "../core/Memory.hpp"
#include "../core/MemoryStream.h"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../localisation/Localisation.h"
#include "../localisation/LocalisationService.h"
#include "../object/Object.h"
#include "../platform/platform.h"
#include "../rct12/SawyerChunkReader.h"
#include "../rct12/SawyerChunkWriter.h"
#include "../scenario/ScenarioRepository.h"
#include "../util/SawyerCoding.h"
#include "../util/Util.h"
#include "Object.h"
#include "ObjectFactory.h"
#include "ObjectList.h"
#include "ObjectManager.h"
#include "RideObject.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

// windows.h defines CP_UTF8
#undef CP_UTF8

using namespace OpenRCT2;

struct ObjectEntryHash
{
    size_t operator()(const rct_object_entry& entry) const
    {
        uint32_t hash = 5381;
        for (auto i : entry.name)
        {
            hash = ((hash << 5) + hash) + i;
        }
        return hash;
    }
};

struct ObjectEntryEqual
{
    bool operator()(const rct_object_entry& lhs, const rct_object_entry& rhs) const
    {
        return memcmp(&lhs.name, &rhs.name, 8) == 0;
    }
};

using ObjectIdentifierMap = std::unordered_map<std::string, size_t>;
using ObjectEntryMap = std::unordered_map<rct_object_entry, size_t, ObjectEntryHash, ObjectEntryEqual>;

class ObjectFileIndex final : public FileIndex<ObjectRepositoryItem>
{
private:
    static constexpr uint32_t MAGIC_NUMBER = 0x5844494F; // OIDX
    static constexpr uint16_t VERSION = 22;
    static constexpr auto PATTERN = "*.dat;*.pob;*.json;*.parkobj";

    IObjectRepository& _objectRepository;

public:
    explicit ObjectFileIndex(IObjectRepository& objectRepository, const IPlatformEnvironment& env)
        : FileIndex(
            "object index", MAGIC_NUMBER, VERSION, env.GetFilePath(PATHID::CACHE_OBJECTS), std::string(PATTERN),
            std::vector<std::string>{
                env.GetDirectoryPath(DIRBASE::OPENRCT2, DIRID::OBJECT),
                env.GetDirectoryPath(DIRBASE::USER, DIRID::OBJECT),
            })
        , _objectRepository(objectRepository)
    {
    }

public:
    std::tuple<bool, ObjectRepositoryItem> Create([[maybe_unused]] int32_t language, const std::string& path) const override
    {
        std::unique_ptr<Object> object;
        auto extension = Path::GetExtension(path);
        if (String::Equals(extension, ".json", true))
        {
            object = ObjectFactory::CreateObjectFromJsonFile(_objectRepository, path);
        }
        else if (String::Equals(extension, ".parkobj", true))
        {
            object = ObjectFactory::CreateObjectFromZipFile(_objectRepository, path);
        }
        else
        {
            object = ObjectFactory::CreateObjectFromLegacyFile(_objectRepository, path.c_str());
        }
        if (object != nullptr)
        {
            ObjectRepositoryItem item = {};
            item.Identifier = object->GetIdentifier();
            item.ObjectEntry = *object->GetObjectEntry();
            item.Path = path;
            item.Name = object->GetName();
            item.Authors = object->GetAuthors();
            item.Sources = object->GetSourceGames();
            object->SetRepositoryItem(&item);
            return std::make_tuple(true, item);
        }
        return std::make_tuple(false, ObjectRepositoryItem());
    }

protected:
    void Serialise(IStream* stream, const ObjectRepositoryItem& item) const override
    {
        stream->WriteValue(item.Identifier);
        stream->WriteValue(item.ObjectEntry);
        stream->WriteString(item.Path);
        stream->WriteString(item.Name);

        uint8_t sourceLength = static_cast<uint8_t>(item.Sources.size());
        stream->WriteValue(sourceLength);
        for (auto source : item.Sources)
        {
            stream->WriteValue(source);
        }

        uint8_t authorsLength = static_cast<uint8_t>(item.Authors.size());
        stream->WriteValue(authorsLength);
        for (const auto& author : item.Authors)
        {
            stream->WriteString(author);
        }

        switch (item.ObjectEntry.GetType())
        {
            case ObjectType::Ride:
                stream->WriteValue<uint8_t>(item.RideInfo.RideFlags);
                for (int32_t i = 0; i < MAX_CATEGORIES_PER_RIDE; i++)
                {
                    stream->WriteValue<uint8_t>(item.RideInfo.RideCategory[i]);
                }
                for (int32_t i = 0; i < MAX_RIDE_TYPES_PER_RIDE_ENTRY; i++)
                {
                    stream->WriteValue<uint8_t>(item.RideInfo.RideType[i]);
                }
                break;
            case ObjectType::SceneryGroup:
                stream->WriteValue<uint16_t>(static_cast<uint16_t>(item.SceneryGroupInfo.Entries.size()));
                for (const auto& entry : item.SceneryGroupInfo.Entries)
                {
                    stream->WriteValue<rct_object_entry>(entry);
                }
                break;
            default:
                // Switch processes only ObjectType::Ride and ObjectType::SceneryGroup
                break;
        }
    }

    ObjectRepositoryItem Deserialise(IStream* stream) const override
    {
        ObjectRepositoryItem item;

        item.Identifier = stream->ReadStdString();
        item.ObjectEntry = stream->ReadValue<rct_object_entry>();
        item.Path = stream->ReadStdString();
        item.Name = stream->ReadStdString();

        auto sourceLength = stream->ReadValue<uint8_t>();
        for (size_t i = 0; i < sourceLength; i++)
        {
            auto value = stream->ReadValue<uint8_t>();
            item.Sources.push_back(static_cast<ObjectSourceGame>(value));
        }

        auto authorsLength = stream->ReadValue<uint8_t>();
        for (size_t i = 0; i < authorsLength; i++)
        {
            auto author = stream->ReadStdString();
            item.Authors.emplace_back(author);
        }

        switch (item.ObjectEntry.GetType())
        {
            case ObjectType::Ride:
                item.RideInfo.RideFlags = stream->ReadValue<uint8_t>();
                for (int32_t i = 0; i < MAX_CATEGORIES_PER_RIDE; i++)
                {
                    item.RideInfo.RideCategory[i] = stream->ReadValue<uint8_t>();
                }
                for (int32_t i = 0; i < MAX_RIDE_TYPES_PER_RIDE_ENTRY; i++)
                {
                    item.RideInfo.RideType[i] = stream->ReadValue<uint8_t>();
                }
                break;
            case ObjectType::SceneryGroup:
            {
                auto numEntries = stream->ReadValue<uint16_t>();
                item.SceneryGroupInfo.Entries = std::vector<rct_object_entry>(numEntries);
                for (size_t i = 0; i < numEntries; i++)
                {
                    item.SceneryGroupInfo.Entries[i] = stream->ReadValue<rct_object_entry>();
                }
                break;
            }
            default:
                // Switch processes only ObjectType::Ride and ObjectType::SceneryGroup
                break;
        }
        return item;
    }

private:
    bool IsTrackReadOnly(const std::string& path) const
    {
        return String::StartsWith(path, SearchPaths[0]) || String::StartsWith(path, SearchPaths[1]);
    }
};

class ObjectRepository final : public IObjectRepository
{
    std::shared_ptr<IPlatformEnvironment> const _env;
    ObjectFileIndex const _fileIndex;
    std::vector<ObjectRepositoryItem> _items;
    ObjectIdentifierMap _newItemMap;
    ObjectEntryMap _itemMap;

public:
    explicit ObjectRepository(const std::shared_ptr<IPlatformEnvironment>& env)
        : _env(env)
        , _fileIndex(*this, *env)
    {
    }

    ~ObjectRepository() final
    {
        ClearItems();
    }

    void LoadOrConstruct(int32_t language) override
    {
        ClearItems();
        auto items = _fileIndex.LoadOrBuild(language);
        AddItems(items);
        SortItems();
    }

    void Construct(int32_t language) override
    {
        auto items = _fileIndex.Rebuild(language);
        AddItems(items);
        SortItems();
    }

    size_t GetNumObjects() const override
    {
        return _items.size();
    }

    const ObjectRepositoryItem* GetObjects() const override
    {
        return _items.data();
    }

    const ObjectRepositoryItem* FindObjectLegacy(const std::string_view& legacyIdentifier) const override
    {
        rct_object_entry entry = {};
        entry.SetName(legacyIdentifier);

        auto kvp = _itemMap.find(entry);
        if (kvp != _itemMap.end())
        {
            return &_items[kvp->second];
        }
        return nullptr;
    }

    const ObjectRepositoryItem* FindObject(std::string_view identifier) const override final
    {
        auto kvp = _newItemMap.find(std::string(identifier));
        if (kvp != _newItemMap.end())
        {
            return &_items[kvp->second];
        }
        return nullptr;
    }

    const ObjectRepositoryItem* FindObject(const rct_object_entry* objectEntry) const override final
    {
        auto kvp = _itemMap.find(*objectEntry);
        if (kvp != _itemMap.end())
        {
            return &_items[kvp->second];
        }
        return nullptr;
    }

    std::unique_ptr<Object> LoadObject(const ObjectRepositoryItem* ori) override
    {
        Guard::ArgumentNotNull(ori, GUARD_LINE);

        auto extension = Path::GetExtension(ori->Path);
        if (String::Equals(extension, ".json", true))
        {
            return ObjectFactory::CreateObjectFromJsonFile(*this, ori->Path);
        }
        else if (String::Equals(extension, ".parkobj", true))
        {
            return ObjectFactory::CreateObjectFromZipFile(*this, ori->Path);
        }
        else
        {
            return ObjectFactory::CreateObjectFromLegacyFile(*this, ori->Path.c_str());
        }
    }

    void RegisterLoadedObject(const ObjectRepositoryItem* ori, Object* object) override
    {
        ObjectRepositoryItem* item = &_items[ori->Id];

        Guard::Assert(item->LoadedObject == nullptr, GUARD_LINE);
        item->LoadedObject = object;
    }

    void UnregisterLoadedObject(const ObjectRepositoryItem* ori, Object* object) override
    {
        ObjectRepositoryItem* item = &_items[ori->Id];
        if (item->LoadedObject == object)
        {
            item->LoadedObject = nullptr;
        }
    }

    void AddObject(const rct_object_entry* objectEntry, const void* data, size_t dataSize) override
    {
        utf8 objectName[9];
        object_entry_get_name_fixed(objectName, sizeof(objectName), objectEntry);

        // Check that the object is loadable before writing it
        auto object = ObjectFactory::CreateObjectFromLegacyData(*this, objectEntry, data, dataSize);
        if (object == nullptr)
        {
            Console::Error::WriteLine("[%s] Unable to export object.", objectName);
        }
        else
        {
            log_verbose("Adding object: [%s]", objectName);
            auto path = GetPathForNewObject(objectName);
            try
            {
                SaveObject(path, objectEntry, data, dataSize);
                ScanObject(path);
            }
            catch (const std::exception&)
            {
                Console::Error::WriteLine("Failed saving object: [%s] to '%s'.", objectName, path.c_str());
            }
        }
    }

    void AddObjectFromFile(const std::string_view& objectName, const void* data, size_t dataSize) override
    {
        log_verbose("Adding object: [%s]", std::string(objectName).c_str());
        auto path = GetPathForNewObject(objectName);
        try
        {
            File::WriteAllBytes(path, data, dataSize);
            ScanObject(path);
        }
        catch (const std::exception&)
        {
            Console::Error::WriteLine("Failed saving object: [%s] to '%s'.", std::string(objectName).c_str(), path.c_str());
        }
    }

    void ExportPackedObject(IStream* stream) override
    {
        auto chunkReader = SawyerChunkReader(stream);

        // Check if we already have this object
        rct_object_entry entry = stream->ReadValue<rct_object_entry>();
        if (FindObject(&entry) != nullptr)
        {
            chunkReader.SkipChunk();
        }
        else
        {
            // Read object and save to new file
            std::shared_ptr<SawyerChunk> chunk = chunkReader.ReadChunk();
            AddObject(&entry, chunk->GetData(), chunk->GetLength());
        }
    }

    void WritePackedObjects(IStream* stream, std::vector<const ObjectRepositoryItem*>& objects) override
    {
        log_verbose("packing %u objects", objects.size());
        for (const auto& object : objects)
        {
            Guard::ArgumentNotNull(object);

            log_verbose("exporting object %.8s", object->ObjectEntry.name);
            if (IsObjectCustom(object))
            {
                WritePackedObject(stream, &object->ObjectEntry);
            }
            else
            {
                log_warning("Refusing to pack vanilla/expansion object \"%s\"", object->ObjectEntry.name);
            }
        }
    }

private:
    void ClearItems()
    {
        _items.clear();
        _newItemMap.clear();
        _itemMap.clear();
    }

    void SortItems()
    {
        std::sort(_items.begin(), _items.end(), [](const ObjectRepositoryItem& a, const ObjectRepositoryItem& b) -> bool {
            return String::Compare(a.Name, b.Name) < 0;
        });

        // Fix the IDs
        for (size_t i = 0; i < _items.size(); i++)
        {
            _items[i].Id = i;
        }

        // Rebuild item map
        _itemMap.clear();
        _newItemMap.clear();
        for (size_t i = 0; i < _items.size(); i++)
        {
            rct_object_entry entry = _items[i].ObjectEntry;
            _itemMap[entry] = i;
            if (!_items[i].Identifier.empty())
            {
                _newItemMap[_items[i].Identifier] = i;
            }
        }
    }

    void AddItems(const std::vector<ObjectRepositoryItem>& items)
    {
        size_t numConflicts = 0;
        for (auto item : items)
        {
            if (!AddItem(item))
            {
                numConflicts++;
            }
        }
        if (numConflicts > 0)
        {
            Console::Error::WriteLine("%zu object conflicts found.", numConflicts);
        }
    }

    bool AddItem(const ObjectRepositoryItem& item)
    {
        auto conflict = FindObject(&item.ObjectEntry);
        if (conflict == nullptr)
        {
            size_t index = _items.size();
            auto copy = item;
            copy.Id = index;
            _items.push_back(copy);
            if (!item.Identifier.empty())
            {
                _newItemMap[item.Identifier] = index;
            }
            _itemMap[item.ObjectEntry] = index;
            return true;
        }
        else
        {
            Console::Error::WriteLine("Object conflict: '%s'", conflict->Path.c_str());
            Console::Error::WriteLine("               : '%s'", item.Path.c_str());
            return false;
        }
    }

    void ScanObject(const std::string& path)
    {
        auto language = LocalisationService_GetCurrentLanguage();
        auto result = _fileIndex.Create(language, path);
        if (std::get<0>(result))
        {
            auto ori = std::get<1>(result);
            AddItem(ori);
        }
    }

    static void SaveObject(
        const std::string_view& path, const rct_object_entry* entry, const void* data, size_t dataSize, bool fixChecksum = true)
    {
        if (fixChecksum)
        {
            uint32_t realChecksum = object_calculate_checksum(entry, data, dataSize);
            if (realChecksum != entry->checksum)
            {
                char objectName[9];
                object_entry_get_name_fixed(objectName, sizeof(objectName), entry);
                log_verbose("[%s] Incorrect checksum, adding salt bytes...", objectName);

                // Calculate the value of extra bytes that can be appended to the data so that the
                // data is then valid for the object's checksum
                size_t extraBytesCount = 0;
                void* extraBytes = CalculateExtraBytesToFixChecksum(realChecksum, entry->checksum, &extraBytesCount);

                // Create new data blob with appended bytes
                size_t newDataSize = dataSize + extraBytesCount;
                uint8_t* newData = Memory::Allocate<uint8_t>(newDataSize);
                uint8_t* newDataSaltOffset = newData + dataSize;
                std::copy_n(static_cast<const uint8_t*>(data), dataSize, newData);
                std::copy_n(static_cast<const uint8_t*>(extraBytes), extraBytesCount, newDataSaltOffset);

                try
                {
                    uint32_t newRealChecksum = object_calculate_checksum(entry, newData, newDataSize);
                    if (newRealChecksum != entry->checksum)
                    {
                        Console::Error::WriteLine("CalculateExtraBytesToFixChecksum failed to fix checksum.");

                        // Save old data form
                        SaveObject(path, entry, data, dataSize, false);
                    }
                    else
                    {
                        // Save new data form
                        SaveObject(path, entry, newData, newDataSize, false);
                    }
                    Memory::Free(newData);
                    Memory::Free(extraBytes);
                }
                catch (const std::exception&)
                {
                    Memory::Free(newData);
                    Memory::Free(extraBytes);
                    throw;
                }
                return;
            }
        }

        // Encode data
        ObjectType objectType = entry->GetType();
        sawyercoding_chunk_header chunkHeader;
        chunkHeader.encoding = object_entry_group_encoding[EnumValue(objectType)];
        chunkHeader.length = static_cast<uint32_t>(dataSize);
        uint8_t* encodedDataBuffer = Memory::Allocate<uint8_t>(0x600000);
        size_t encodedDataSize = sawyercoding_write_chunk_buffer(
            encodedDataBuffer, reinterpret_cast<const uint8_t*>(data), chunkHeader);

        // Save to file
        try
        {
            auto fs = FileStream(std::string(path), FILE_MODE_WRITE);
            fs.Write(entry, sizeof(rct_object_entry));
            fs.Write(encodedDataBuffer, encodedDataSize);

            Memory::Free(encodedDataBuffer);
        }
        catch (const std::exception&)
        {
            Memory::Free(encodedDataBuffer);
            throw;
        }
    }

    static void* CalculateExtraBytesToFixChecksum(int32_t currentChecksum, int32_t targetChecksum, size_t* outSize)
    {
        // Allocate 11 extra bytes to manipulate the checksum
        uint8_t* salt = Memory::Allocate<uint8_t>(11);
        if (outSize != nullptr)
            *outSize = 11;

        // Next work out which bits need to be flipped to make the current checksum match the one in the file
        // The bitwise rotation compensates for the rotation performed during the checksum calculation*/
        int32_t bitsToFlip = targetChecksum ^ ((currentChecksum << 25) | (currentChecksum >> 7));

        // Each set bit encountered during encoding flips one bit of the resulting checksum (so each bit of the checksum is an
        // XOR of bits from the file). Here, we take each bit that should be flipped in the checksum and set one of the bits in
        // the data that maps to it. 11 bytes is the minimum needed to touch every bit of the checksum - with less than that,
        // you wouldn't always be able to make the checksum come out to the desired target
        salt[0] = (bitsToFlip & 0x00000001) << 7;
        salt[1] = ((bitsToFlip & 0x00200000) >> 14);
        salt[2] = ((bitsToFlip & 0x000007F8) >> 3);
        salt[3] = ((bitsToFlip & 0xFF000000) >> 24);
        salt[4] = ((bitsToFlip & 0x00100000) >> 13);
        salt[5] = (bitsToFlip & 0x00000004) >> 2;
        salt[6] = 0;
        salt[7] = ((bitsToFlip & 0x000FF000) >> 12);
        salt[8] = (bitsToFlip & 0x00000002) >> 1;
        salt[9] = (bitsToFlip & 0x00C00000) >> 22;
        salt[10] = (bitsToFlip & 0x00000800) >> 11;

        return salt;
    }

    std::string GetPathForNewObject(const std::string_view& name)
    {
        // Get object directory and create it if it doesn't exist
        auto userObjPath = _env->GetDirectoryPath(DIRBASE::USER, DIRID::OBJECT);
        Path::CreateDirectory(userObjPath);

        // Find a unique file name
        auto fileName = GetFileNameForNewObject(name);
        auto fullPath = Path::Combine(userObjPath, fileName + ".DAT");
        auto counter = 1U;
        while (File::Exists(fullPath))
        {
            counter++;
            fullPath = Path::Combine(userObjPath, String::StdFormat("%s-%02X.DAT", fileName.c_str(), counter));
        }

        return fullPath;
    }

    std::string GetFileNameForNewObject(const std::string_view& name)
    {
        // Trim name
        char normalisedName[9] = { 0 };
        auto maxLength = std::min<size_t>(name.size(), 8);
        for (size_t i = 0; i < maxLength; i++)
        {
            if (name[i] != ' ')
            {
                normalisedName[i] = toupper(name[i]);
            }
            else
            {
                normalisedName[i] = '\0';
                break;
            }
        }

        // Convert to UTF-8 filename
        return String::Convert(normalisedName, CODE_PAGE::CP_1252, CODE_PAGE::CP_UTF8);
    }

    void WritePackedObject(OpenRCT2::IStream* stream, const rct_object_entry* entry)
    {
        const ObjectRepositoryItem* item = FindObject(entry);
        if (item == nullptr)
        {
            throw std::runtime_error(String::StdFormat("Unable to find object '%.8s'", entry->name));
        }

        // Read object data from file
        auto fs = OpenRCT2::FileStream(item->Path, OpenRCT2::FILE_MODE_OPEN);
        auto fileEntry = fs.ReadValue<rct_object_entry>();
        if (!object_entry_compare(entry, &fileEntry))
        {
            throw std::runtime_error("Header found in object file does not match object to pack.");
        }
        auto chunkReader = SawyerChunkReader(&fs);
        auto chunk = chunkReader.ReadChunk();

        // Write object data to stream
        auto chunkWriter = SawyerChunkWriter(stream);
        stream->WriteValue(*entry);
        chunkWriter.WriteChunk(chunk.get());
    }
};

std::unique_ptr<IObjectRepository> CreateObjectRepository(const std::shared_ptr<IPlatformEnvironment>& env)
{
    return std::make_unique<ObjectRepository>(env);
}

bool IsObjectCustom(const ObjectRepositoryItem* object)
{
    Guard::ArgumentNotNull(object);

    // Do not count our new object types as custom yet, otherwise the game
    // will try to pack them into saved games.
    auto type = object->ObjectEntry.GetType();
    if (type > ObjectType::ScenarioText)
    {
        return false;
    }

    switch (object->GetFirstSourceGame())
    {
        case ObjectSourceGame::RCT2:
        case ObjectSourceGame::WackyWorlds:
        case ObjectSourceGame::TimeTwister:
        case ObjectSourceGame::OpenRCT2Official:
            return false;
        default:
            return true;
    }
}

const rct_object_entry* object_list_find(rct_object_entry* entry)
{
    const rct_object_entry* result = nullptr;
    auto& objRepo = GetContext()->GetObjectRepository();
    auto item = objRepo.FindObject(entry);
    if (item != nullptr)
    {
        result = &item->ObjectEntry;
    }
    return result;
}

std::unique_ptr<Object> object_repository_load_object(const rct_object_entry* objectEntry)
{
    std::unique_ptr<Object> object;
    auto& objRepository = GetContext()->GetObjectRepository();
    const ObjectRepositoryItem* ori = objRepository.FindObject(objectEntry);
    if (ori != nullptr)
    {
        object = objRepository.LoadObject(ori);
        if (object != nullptr)
        {
            object->Load();
        }
    }
    return object;
}

void scenario_translate(scenario_index_entry* scenarioEntry)
{
    rct_string_id localisedStringIds[3];
    if (language_get_localised_scenario_strings(scenarioEntry->name, localisedStringIds))
    {
        if (localisedStringIds[0] != STR_NONE)
        {
            String::Set(scenarioEntry->name, sizeof(scenarioEntry->name), language_get_string(localisedStringIds[0]));
        }
        if (localisedStringIds[2] != STR_NONE)
        {
            String::Set(scenarioEntry->details, sizeof(scenarioEntry->details), language_get_string(localisedStringIds[2]));
        }
    }
}

size_t object_repository_get_items_count()
{
    auto& objectRepository = GetContext()->GetObjectRepository();
    return objectRepository.GetNumObjects();
}

const ObjectRepositoryItem* object_repository_get_items()
{
    auto& objectRepository = GetContext()->GetObjectRepository();
    return objectRepository.GetObjects();
}

const ObjectRepositoryItem* object_repository_find_object_by_entry(const rct_object_entry* entry)
{
    auto& objectRepository = GetContext()->GetObjectRepository();
    return objectRepository.FindObject(entry);
}

const ObjectRepositoryItem* object_repository_find_object_by_name(const char* name)
{
    auto& objectRepository = GetContext()->GetObjectRepository();
    return objectRepository.FindObjectLegacy(name);
}

bool object_entry_compare(const rct_object_entry* a, const rct_object_entry* b)
{
    // If an official object don't bother checking checksum
    if ((a->flags & 0xF0) || (b->flags & 0xF0))
    {
        if (a->GetType() != b->GetType())
        {
            return false;
        }
        int32_t match = memcmp(a->name, b->name, 8);
        if (match)
        {
            return false;
        }
    }
    else
    {
        if (a->flags != b->flags)
        {
            return false;
        }
        int32_t match = memcmp(a->name, b->name, 8);
        if (match)
        {
            return false;
        }
        if (a->checksum != b->checksum)
        {
            return false;
        }
    }
    return true;
}

int32_t object_calculate_checksum(const rct_object_entry* entry, const void* data, size_t dataLength)
{
    const uint8_t* entryBytePtr = reinterpret_cast<const uint8_t*>(entry);

    uint32_t checksum = 0xF369A75B;
    checksum ^= entryBytePtr[0];
    checksum = rol32(checksum, 11);
    for (int32_t i = 4; i < 12; i++)
    {
        checksum ^= entryBytePtr[i];
        checksum = rol32(checksum, 11);
    }

    const uint8_t* dataBytes = reinterpret_cast<const uint8_t*>(data);
    const size_t dataLength32 = dataLength - (dataLength & 31);
    for (size_t i = 0; i < 32; i++)
    {
        for (size_t j = i; j < dataLength32; j += 32)
        {
            checksum ^= dataBytes[j];
        }
        checksum = rol32(checksum, 11);
    }
    for (size_t i = dataLength32; i < dataLength; i++)
    {
        checksum ^= dataBytes[i];
        checksum = rol32(checksum, 11);
    }

    return static_cast<int32_t>(checksum);
}

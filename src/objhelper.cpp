/*
    * WinDBG Anti-RootKit extension
    * Copyright � 2013-2018  Vyacheslav Rusakoff
    * 
    * This program is free software: you can redistribute it and/or modify
    * it under the terms of the GNU General Public License as published by
    * the Free Software Foundation, either version 3 of the License, or
    * (at your option) any later version.
    * 
    * This program is distributed in the hope that it will be useful,
    * but WITHOUT ANY WARRANTY; without even the implied warranty of
    * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    * GNU General Public License for more details.
    * 
    * You should have received a copy of the GNU General Public License
    * along with this program.  If not, see <http://www.gnu.org/licenses/>.

    * This work is licensed under the terms of the GNU GPL, version 3.  See
    * the COPYING file in the top-level directory.
*/

#include <string>
#include <iomanip>
#include <algorithm>
#include <utility>
#include <memory>

#include "objhelper.hpp"
#include "wdbgark.hpp"
#include "manipulators.hpp"
#include "strings.hpp"

namespace wa {

uint64_t ExFastRefGetObject(uint64_t fast_ref) {
    if ( g_Ext->IsCurMachine32() ) {
        return fast_ref & ~MAX_FAST_REFS_X86;
    } else {
        return fast_ref & ~MAX_FAST_REFS_X64;
    }
}
//////////////////////////////////////////////////////////////////////////
// base object manager helper class
//////////////////////////////////////////////////////////////////////////
WDbgArkObjHelper::WDbgArkObjHelper(const std::shared_ptr<WDbgArkSymCache> &sym_cache) : m_sym_cache(sym_cache) {
    // determine object header format
    uint32_t type_index_offset = 0;

    // old header format
    if ( GetFieldOffset("nt!_OBJECT_HEADER", "TypeIndex", reinterpret_cast<PULONG>(&type_index_offset)) != 0 ) {
        m_inited = true;
    } else {
        m_object_header_old = false;  // new header format

        if ( !m_sym_cache->GetSymbolOffset("nt!ObpInfoMaskToOffset", true, &m_ObpInfoMaskToOffset) ) {
            err << wa::showminus << __FUNCTION__ << ": failed to find nt!ObpInfoMaskToOffset" << endlerr;
            return;
        }

        if ( !m_sym_cache->GetSymbolOffset("nt!ObTypeIndexTable", true, &m_ObTypeIndexTableOffset) ) {
            err << wa::showminus << __FUNCTION__ << ": failed to find nt!ObTypeIndexTable" << endlerr;
            return;
        }

        // Windows 10+
        uint64_t header_cookie_offset = 0;

        if ( m_sym_cache->GetSymbolOffset("nt!ObHeaderCookie", true, &header_cookie_offset) ) {
            m_ObHeaderCookie = ExtRemoteData(header_cookie_offset, sizeof(m_ObHeaderCookie)).GetUchar();
        }

        m_inited = true;
    }
}

// TODO(swwwolf): make refactoring
WDbgArkObjHelper::ObjectsInfoResult WDbgArkObjHelper::GetObjectsInfo(const uint64_t directory_address,
                                                                     const std::string &root_path,
                                                                     const bool recursive) {
    uint64_t offset = directory_address;
    ObjectsInformation info;

    try {
        if ( !offset ) {
            if ( !m_sym_cache->GetSymbolOffset("nt!ObpRootDirectoryObject", true, &offset) ) {
                err << wa::showminus << __FUNCTION__ << ": failed to get nt!ObpRootDirectoryObject" << endlerr;
                return std::make_pair(E_UNEXPECTED, info);
            } else {
                ExtRemoteData directory_object_ptr(offset, g_Ext->m_PtrSize);
                offset = directory_object_ptr.GetPtr();
            }
        }

        const std::string obj_dir("nt!_OBJECT_DIRECTORY");
        ExtRemoteTyped directory_object(obj_dir.c_str(), offset, false, m_sym_cache->GetCookieCache(obj_dir), nullptr);

        auto buckets = directory_object.Field("HashBuckets");
        const ULONG num_buckets = buckets.GetTypeSize() / g_Ext->m_PtrSize;

        for ( ULONG i = 0; i < num_buckets; i++ ) {
            if ( !buckets.m_Offset ) {
                continue;
            }

            try {
                for ( auto directory_entry = *buckets[i];
                      directory_entry.m_Offset;
                      directory_entry = *directory_entry.Field("ChainLink") ) {
                    try {
                        ObjectInfo obj_info;

                        obj_info.directory_object = directory_object;
                        obj_info.object = *directory_entry.Field("Object");

                        const auto [result, name] = GetObjectName(obj_info.object);

                        obj_info.obj_name = name;
                        obj_info.full_path = root_path + obj_info.obj_name;

                        const auto [result_type, type_name] = GetObjectTypeName(obj_info.object);

                        obj_info.type_name = type_name;

                        // workaround for an infinite loop (broken crash dump with broken object directory)
                        const auto [iter_first, iter_second] = info.equal_range(obj_info.object.m_Offset);

                        // not in map
                        if ( iter_first == iter_second ) {
                            info.insert(iter_first, std::make_pair(obj_info.object.m_Offset, obj_info));
                        } else {
                            // already in map and this is strange
                            break;
                        }

                        if ( recursive && obj_info.type_name == "Directory" ) {
                            obj_info.full_path += R"(\)";

                            const auto [result_info, recursive_info] = GetObjectsInfo(obj_info.object.m_Offset,
                                                                                      obj_info.full_path,
                                                                                      recursive);

                            if ( SUCCEEDED(result_info) ) {
                                info.insert(std::begin(recursive_info), std::end(recursive_info));
                            }
                        }
                    } catch ( const ExtRemoteException &Ex ) {
                        warn << wa::showqmark << __FUNCTION__ << ": " << Ex.GetMessage() << endlwarn;
                        continue;
                    }
                }
            } catch ( const ExtRemoteException &Ex ) {
                warn << wa::showqmark << __FUNCTION__ << ": " << Ex.GetMessage() << endlwarn;
                continue;
            }
        }
    } catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
        return std::make_pair(Ex.GetStatus(), info);
    }

    return std::make_pair(S_OK, info);
}

uint64_t WDbgArkObjHelper::FindObjectByName(const std::string &object_name,
                                            const uint64_t directory_address,
                                            const std::string &root_path,
                                            const bool recursive) {
    if ( object_name.empty() ) {
        err << wa::showminus << __FUNCTION__ << ": invalid object name" << endlerr;
        return 0ULL;
    }

    const auto [result, info] = GetObjectsInfo(directory_address, root_path, recursive);

    if ( FAILED(result) ) {
        err << wa::showminus << __FUNCTION__ << ": GetObjectsInfo failed" << endlerr;
        return 0ULL;
    }

    std::string compare_full_path = root_path + object_name;

    if ( root_path == R"(\)" && object_name[0] == '\\' ) {
        compare_full_path = object_name;
    }

    compare_full_path = wa::tolower(compare_full_path);

    for ( const auto [offset, object_info] : info ) {
        std::string full_path = wa::tolower(object_info.full_path);

        if ( full_path == compare_full_path ) {
            return object_info.object.m_Offset;
        }
    }

    return 0ULL;
}

std::pair<HRESULT, ExtRemoteTyped> WDbgArkObjHelper::GetObjectHeader(const ExtRemoteTyped &object) {
    ExtRemoteTyped object_header;

    try {
        uint32_t offset = 0;

        if ( GetFieldOffset("nt!_OBJECT_HEADER", "Body", reinterpret_cast<PULONG>(&offset)) != 0 ) {
            err << wa::showminus << __FUNCTION__ << ": GetFieldOffset failed" << endlerr;
            return std::make_pair(E_UNEXPECTED, object_header);
        }

        if ( !offset ) {
            err << wa::showminus << __FUNCTION__ << ": body field is missing in nt!_OBJECT_HEADER" << endlerr;
            return std::make_pair(E_UNEXPECTED, object_header);
        }

        const std::string obj_hdr("nt!_OBJECT_HEADER");
        object_header.Set(obj_hdr.c_str(),
                          object.m_Offset - offset,
                          false,
                          m_sym_cache->GetCookieCache(obj_hdr),
                          nullptr);
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
        return std::make_pair(Ex.GetStatus(), object_header);
    }

    return std::make_pair(S_OK, object_header);
}

bool WDbgArkObjHelper::HasObjectHeaderNameInfo(const ExtRemoteTyped &object_header) const {
    try {
        auto loc_object_header = const_cast<ExtRemoteTyped&>(object_header);

        if ( m_object_header_old ) {
            if ( loc_object_header.Field("NameInfoOffset").GetUchar() ) {
                return true;
            }
        } else {
            if ( loc_object_header.Field("InfoMask").GetUchar() & HeaderNameInfoFlag ) {
                return true;
            }
        }
    } catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
    }

    return false;
}

std::pair<HRESULT, ExtRemoteTyped> WDbgArkObjHelper::GetObjectHeaderNameInfo(const ExtRemoteTyped &object_header) {
    ExtRemoteTyped object_header_name_info;

    try {
        auto loc_object_header = const_cast<ExtRemoteTyped&>(object_header);
        const std::string obj_hdr_name_info("nt!_OBJECT_HEADER_NAME_INFO");

        if ( m_object_header_old ) {
            auto name_info_offset = loc_object_header.Field("NameInfoOffset");

            if ( name_info_offset.GetUchar() ) {
                object_header_name_info.Set(obj_hdr_name_info.c_str(),
                                            object_header.m_Offset - name_info_offset.GetUchar(),
                                            false,
                                            m_sym_cache->GetCookieCache(obj_hdr_name_info),
                                            nullptr);

                return std::make_pair(S_OK, object_header_name_info);
            }
        } else {
            if ( m_ObpInfoMaskToOffset ) {
                auto info_mask = loc_object_header.Field("InfoMask");

                if ( info_mask.GetUchar() & HeaderNameInfoFlag ) {
                    const auto obp_offset = info_mask.GetUchar() & (HeaderNameInfoFlag | (HeaderNameInfoFlag - 1));
                    ExtRemoteData name_info_mask_to_offset(m_ObpInfoMaskToOffset + obp_offset, sizeof(uint8_t));

                    object_header_name_info.Set(obj_hdr_name_info.c_str(),
                                                loc_object_header.m_Offset - name_info_mask_to_offset.GetUchar(),
                                                false,
                                                m_sym_cache->GetCookieCache(obj_hdr_name_info),
                                                nullptr);

                    return std::make_pair(S_OK, object_header_name_info);
                }
            }
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
        return std::make_pair(Ex.GetStatus(), object_header_name_info);
    }

    return std::make_pair(E_UNEXPECTED, object_header_name_info);
}

std::pair<HRESULT, std::string> WDbgArkObjHelper::GetObjectName(const ExtRemoteTyped &object) {
    std::string output_string("*UNKNOWN*");

    const auto [result, header] = GetObjectHeader(object);

    if ( FAILED(result) ) {
        err << wa::showminus << __FUNCTION__ << ": failed to get object header" << endlerr;
        return std::make_pair(result, output_string);
    }

    if ( !HasObjectHeaderNameInfo(header) ) {
        return std::make_pair(S_OK, std::string());
    }

    auto [result_info, name_info] = GetObjectHeaderNameInfo(header);

    if ( FAILED(result_info) ) {
        err << wa::showminus << __FUNCTION__ << ": failed to get object header name info" << endlerr;
        return std::make_pair(result_info, output_string);
    }

    const auto [result_unicode, name] = UnicodeStringStructToString(name_info.Field("Name"));

    if ( FAILED(result_unicode) ) {
        return std::make_pair(result_unicode, output_string);
    }

    return std::make_pair(result_unicode, wstring_to_string(name));     // TODO(swwwolf): do not convert
}

std::pair<HRESULT, ExtRemoteTyped> WDbgArkObjHelper::GetObjectType(const ExtRemoteTyped &object) {
    ExtRemoteTyped object_type;

    try {
        auto [result, header] = GetObjectHeader(object);

        if ( FAILED(result) ) {
            err << wa::showminus << __FUNCTION__ << ": failed to get object header" << endlerr;
            return std::make_pair(result, object_type);
        }

        if ( m_object_header_old ) {
            object_type = header.Field("Type");
        } else {
            uint8_t type_index = 0;

            if ( m_ObHeaderCookie ) {
                type_index = (m_ObHeaderCookie ^ header.Field("TypeIndex").GetUchar()) ^ \
                             static_cast<uint8_t>(header.m_Offset >> 8);
            } else {
                type_index = header.Field("TypeIndex").GetUchar();
            }

            ExtRemoteData object_type_data(m_ObTypeIndexTableOffset + type_index * g_Ext->m_PtrSize, g_Ext->m_PtrSize);

            const std::string obj_type("nt!_OBJECT_TYPE");
            object_type.Set(obj_type.c_str(),
                            object_type_data.GetPtr(),
                            false,
                            m_sym_cache->GetCookieCache(obj_type),
                            nullptr);
        }
    }
    catch ( const ExtRemoteException &Ex ) {
        err << wa::showminus << __FUNCTION__ << ": " << Ex.GetMessage() << endlerr;
        return std::make_pair(Ex.GetStatus(), object_type);
    }

    return std::make_pair(S_OK, object_type);
}

std::pair<HRESULT, std::string> WDbgArkObjHelper::GetObjectTypeName(const ExtRemoteTyped &object) {
    std::string output_string("*UNKNOWN*");

    auto [result, type] = GetObjectType(object);

    if ( FAILED(result) ) {
        err << wa::showminus << __FUNCTION__ << ": failed to get object type" << endlerr;
        return std::make_pair(result, output_string);
    }

    const auto [result_unicode, name] = UnicodeStringStructToString(type.Field("Name"));

    if ( FAILED(result_unicode) ) {
        return std::make_pair(result_unicode, output_string);
    }

    return std::make_pair(result_unicode, wstring_to_string(name));     // TODO(swwwolf): do not convert
}

//////////////////////////////////////////////////////////////////////////
// driver object helper class
//////////////////////////////////////////////////////////////////////////
WDbgArkDrvObjHelper::WDbgArkDrvObjHelper(const std::shared_ptr<WDbgArkSymCache> &sym_cache,
                                         const ExtRemoteTyped &driver) : WDbgArkObjHelper(sym_cache),
                                                                         m_driver(driver) {}

WDbgArkDrvObjHelper::Table WDbgArkDrvObjHelper::GetMajorTable() {
    Table table;

    try {
        auto major_table = m_driver.Field("MajorFunction");

        for ( size_t i = 0; i < m_major_table_name.size(); i++ ) {
            table.push_back({ major_table[static_cast<int64_t>(i)].GetPtr(), m_major_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkDrvObjHelper::Table WDbgArkDrvObjHelper::GetFastIoTable() {
    Table table;

    try {
        auto fast_io_dispatch = m_driver.Field("FastIoDispatch");
        auto fast_io_dispatch_ptr = fast_io_dispatch.GetPtr();

        if ( fast_io_dispatch_ptr != 0ULL ) {
            fast_io_dispatch_ptr = reinterpret_cast<uint64_t>RtlOffsetToPointer(
                fast_io_dispatch_ptr,
                fast_io_dispatch.GetFieldOffset("FastIoCheckIfPossible"));

            for ( size_t i = 0; i < m_fast_io_table_name.size(); i++ ) {
                ExtRemoteData fast_io_dispatch_data(fast_io_dispatch_ptr + i * g_Ext->m_PtrSize, g_Ext->m_PtrSize);
                table.push_back({ fast_io_dispatch_data.GetPtr(), m_fast_io_table_name[i] });
            }
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkDrvObjHelper::Table WDbgArkDrvObjHelper::GetFsFilterCbTable() {
    Table table;

    try {
        if ( m_driver.Field("DriverExtension").GetPtr() ) {
            auto fs_filter_callbacks = m_driver.Field("DriverExtension").Field("FsFilterCallbacks");
            auto fs_filter_callbacks_ptr = fs_filter_callbacks.GetPtr();

            if ( fs_filter_callbacks_ptr != 0ULL ) {
                fs_filter_callbacks_ptr = reinterpret_cast<uint64_t>RtlOffsetToPointer(
                    fs_filter_callbacks_ptr,
                    fs_filter_callbacks.GetFieldOffset("PreAcquireForSectionSynchronization"));

                for ( size_t i = 0; i < m_fs_filter_cb_table_name.size(); i++ ) {
                    ExtRemoteData fs_filter_callbacks_data(fs_filter_callbacks_ptr + i * g_Ext->m_PtrSize,
                                                           g_Ext->m_PtrSize);
                    table.push_back({ fs_filter_callbacks_data.GetPtr(), m_fs_filter_cb_table_name[i] });
                }
            }
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkClassDrvObjHelper::WDbgArkClassDrvObjHelper(const std::shared_ptr<WDbgArkSymCache> &sym_cache,
                                                   const ExtRemoteTyped &driver)
    : WDbgArkDrvObjHelper(sym_cache, driver) {
    try {
        ExtCaptureOutputA ignore_output;
        ignore_output.Start();

        if ( !m_sym_cache->GetTypeSize("classpnp!_CLASS_DRIVER_EXTENSION") ) {
            return;
        }

        ignore_output.Stop();

        m_sym_cache->GetSymbolOffset("classpnp!ClassInitialize", true, &m_class_clientid);

        if ( m_class_clientid != 0ULL ) {
            m_has_class_driver_extension = GetClassDriverExtension();
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    } catch ( const ExtException& ) {
        __noop;
    }
}

std::string WDbgArkClassDrvObjHelper::GetClassExtensionDmlCommand() const {
    std::stringstream object_command;

    object_command << R"(<exec cmd="dtx classpnp!_CLASS_DRIVER_EXTENSION )";
    object_command << std::hex << std::showbase << m_class_driver_extension.m_Offset << R"(">)";
    object_command << std::hex << std::showbase << m_class_driver_extension.m_Offset << "</exec>";

    return object_command.str();
}

WDbgArkClassDrvObjHelper::Table WDbgArkClassDrvObjHelper::GetInitDataTable() {
    Table table;

    try {
        auto ptr = Field("InitData.ClassAddDevice").m_Offset;

        for ( size_t i = 0; i < m_init_data_table_name.size(); i++ ) {
            ExtRemoteData data(ptr + i * g_Ext->m_PtrSize, g_Ext->m_PtrSize);
            table.push_back({ data.GetPtr(), m_init_data_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkClassDrvObjHelper::Table WDbgArkClassDrvObjHelper::GetInitDataFdoDataTable() {
    Table table;

    try {
        auto ptr = Field("InitData.FdoData.ClassError").m_Offset;

        for ( size_t i = 0; i < m_init_data_fdo_data_table_name.size(); i++ ) {
            ExtRemoteData data(ptr + i * g_Ext->m_PtrSize, g_Ext->m_PtrSize);
            table.push_back({ data.GetPtr(), m_init_data_fdo_data_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkClassDrvObjHelper::Table WDbgArkClassDrvObjHelper::GetInitDataFdoDataWmiTable() {
    Table table;

    try {
        auto ptr = Field("InitData.FdoData.ClassWmiInfo.ClassQueryWmiRegInfo").m_Offset;

        for ( size_t i = 0; i < m_init_data_fdo_data_wmi_table_name.size(); i++ ) {
            ExtRemoteData data(ptr + i * g_Ext->m_PtrSize, g_Ext->m_PtrSize);
            table.push_back({ data.GetPtr(), m_init_data_fdo_data_wmi_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkClassDrvObjHelper::Table WDbgArkClassDrvObjHelper::GetInitDataPdoDataTable() {
    Table table;

    try {
        auto ptr = Field("InitData.PdoData.ClassError").m_Offset;

        for ( size_t i = 0; i < m_init_data_pdo_data_table_name.size(); i++ ) {
            ExtRemoteData data(ptr + i * g_Ext->m_PtrSize, g_Ext->m_PtrSize);
            table.push_back({ data.GetPtr(), m_init_data_pdo_data_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkClassDrvObjHelper::Table WDbgArkClassDrvObjHelper::GetInitDataPdoDataWmiTable() {
    Table table;

    try {
        auto ptr = Field("InitData.PdoData.ClassWmiInfo.ClassQueryWmiRegInfo").m_Offset;

        for ( size_t i = 0; i < m_init_data_pdo_data_wmi_table_name.size(); i++ ) {
            ExtRemoteData data(ptr + i * g_Ext->m_PtrSize, g_Ext->m_PtrSize);
            table.push_back({ data.GetPtr(), m_init_data_pdo_data_wmi_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkClassDrvObjHelper::Table WDbgArkClassDrvObjHelper::GetDeviceMajorFunctionTable() {
    Table table;

    try {
        auto major_table = Field("DeviceMajorFunctionTable");

        for ( size_t i = 0; i < m_major_table_name.size(); i++ ) {
            table.push_back({ major_table[static_cast<int64_t>(i)].GetPtr(), m_major_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

WDbgArkClassDrvObjHelper::Table WDbgArkClassDrvObjHelper::GetMpDeviceMajorFunctionTable() {
    Table table;

    try {
        auto major_table = Field("MpDeviceMajorFunctionTable");

        for ( size_t i = 0; i < m_major_table_name.size(); i++ ) {
            table.push_back({ major_table[static_cast<int64_t>(i)].GetPtr(), m_major_table_name[i] });
        }
    } catch ( const ExtRemoteException& ) {
        __noop;
    }

    return table;
}

bool WDbgArkClassDrvObjHelper::GetClassDriverExtension() {
    auto driver_extension = m_driver.Field("DriverExtension");

    if ( !driver_extension.GetPtr() ) {
        return false;
    }

    auto client_driver_extension = driver_extension.Field("ClientDriverExtension");

    while ( client_driver_extension.GetPtr() != 0ULL ) {
        if ( client_driver_extension.Field("ClientIdentificationAddress").GetPtr() == m_class_clientid ) {
            break;
        }

        client_driver_extension = client_driver_extension.Field("NextExtension");
    }

    if ( !client_driver_extension.GetPtr() ) {
        return false;
    }

    const auto offset = reinterpret_cast<uint64_t>RtlOffsetToPointer(client_driver_extension.GetPtr(),
                                                                     (*client_driver_extension).GetTypeSize());

    const std::string class_ext("classpnp!_CLASS_DRIVER_EXTENSION");
    m_class_driver_extension.Set(class_ext.c_str(), offset, false, m_sym_cache->GetCookieCache(class_ext), nullptr);
    return true;
}

}   // namespace wa

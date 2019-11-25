// DirectoryListNode.cpp
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "DirectoryListNode.h"

#include "Tools/FBuild/FBuildCore/FBuild.h"
#include "Tools/FBuild/FBuildCore/FLog.h"
#include "Tools/FBuild/FBuildCore/Graph/NodeGraph.h"

// Core
#include "Core/FileIO/FileIO.h"
#include "Core/FileIO/FileStream.h"
#include "Core/FileIO/MemoryStream.h"
#include "Core/FileIO/PathUtils.h"
#include "Core/Math/xxHash.h"
#include "Core/Strings/AStackString.h"

// Reflection
//------------------------------------------------------------------------------
REFLECT_NODE_BEGIN( DirectoryListNode, Node, MetaNone() )
    REFLECT( m_Path,                    "Path",             MetaNone() )
    REFLECT_ARRAY( m_Patterns,          "Patterns",         MetaNone() )
    REFLECT_ARRAY( m_ExcludePaths,      "ExcludePaths",     MetaNone() )
    REFLECT_ARRAY( m_FilesToExclude,    "FilesToExclude",   MetaNone() )
    REFLECT_ARRAY( m_ExcludePatterns,   "ExcludePatterns",  MetaNone() )
    REFLECT( m_Recursive,               "Recursive",        MetaNone() )
REFLECT_END( DirectoryListNode )

// CONSTRUCTOR
//------------------------------------------------------------------------------
DirectoryListNode::DirectoryListNode()
    : Node( AString::GetEmpty(), Node::DIRECTORY_LIST_NODE, Node::FLAG_ALWAYS_BUILD )
    , m_Recursive( true )
{
    m_LastBuildTimeMs = 100;
}

// Initialize
//------------------------------------------------------------------------------
/*virtual*/ bool DirectoryListNode::Initialize( NodeGraph & /*nodeGraph*/, const BFFIterator & /*iter*/, const Function * /*function*/ )
{
    ASSERT( ( m_Recursive == true ) || ( m_Recursive == false ) );

    // ensure name is correctly formatted
    //   path|[patterns]|recursive|[excludePath]
    ASSERT( m_Name.BeginsWith( m_Path ) );
    ASSERT( m_Name[ m_Path.GetLength() ] == '|' );
    ASSERT( m_Patterns.IsEmpty() || ( m_Name.Find( m_Patterns[ 0 ].Get() ) == m_Name.Get() + m_Path.GetLength() + 1 ) );
    ASSERT( ( m_Recursive && m_Name.Find( "|true|" ) ) ||
            ( !m_Recursive && m_Name.Find( "|false|" ) ) );

    // paths must have trailing slash
    ASSERT( m_Path.EndsWith( NATIVE_SLASH ) );

    // make sure exclusion path has trailing slash if provided
    #if defined( ASSERTS_ENABLED )
        for ( const AString & excludePath : m_ExcludePaths )
        {
            ASSERT( excludePath.EndsWith( NATIVE_SLASH ) );
        }
    #endif

    return true;
}

// DESTRUCTOR
//------------------------------------------------------------------------------
DirectoryListNode::~DirectoryListNode() = default;

// FormatName
//------------------------------------------------------------------------------
/*static*/ void DirectoryListNode::FormatName( const AString & path,
                                               const Array< AString > * patterns,
                                               bool recursive,
                                               const Array< AString > & excludePaths,
                                               const Array< AString > & excludeFiles,
                                               const Array< AString > & excludePatterns,
                                               AString & result )
{
    ASSERT( path.EndsWith( NATIVE_SLASH ) );
    AStackString<> patternString;
    if ( patterns )
    {
        const size_t numPatterns = patterns->GetSize();
        for ( size_t i=0; i<numPatterns; ++i )
        {
            if ( i > 0 )
            {
                patternString += '<';
            }
            patternString += (*patterns)[ i ];
        }
    }
    result.Format( "%s|%s|%s|", path.Get(),
                                  patternString.Get(),
                                  recursive ? "true" : "false" );

    const AString * const end = excludePaths.End();
    for ( const AString * it = excludePaths.Begin(); it!=end; ++it )
    {
        const AString & excludePath = *it;
        ASSERT( excludePath.EndsWith( NATIVE_SLASH ) );
        result += excludePath;
        result += '<';
    }

    if ( !excludeFiles.IsEmpty() )
    {
        result += '|';
        const AString * const endFiles = excludeFiles.End();
        for ( const AString * itFiles = excludeFiles.Begin(); itFiles != endFiles; ++itFiles )
        {
            const AString & excludedFile = *itFiles;
            result += excludedFile;
            result += '<';
        }
    }

    if ( !excludePatterns.IsEmpty() )
    {
        result += '|';
        const AString * const endPatterns = excludePatterns.End();
        for ( const AString * itPatterns = excludePatterns.Begin(); itPatterns != endPatterns; ++itPatterns )
        {
            const AString & excludedPattern = *itPatterns;
            result += excludedPattern;
            result += '<';
        }
    }
}

// DoBuild
//------------------------------------------------------------------------------
/*virtual*/ Node::BuildResult DirectoryListNode::DoBuild( Job * UNUSED( job ) )
{
    // NOTE: The DirectoryListNode makes no assumptions about whether no files
    // is an error or not.  That's up to the dependent nodes to decide.

    Array< FileIO::FileInfo > files( 4096, true );
    FileIO::GetFilesEx( m_Path, &m_Patterns, m_Recursive, &files );

    m_Files.SetCapacity( files.GetSize() );

    // filter exclusions
    const FileIO::FileInfo * const end = files.End();
    for ( const FileIO::FileInfo * it = files.Begin(); it != end; it++ )
    {
        bool excluded = false;

        // filter excluded paths
        const AString * const eEnd = m_ExcludePaths.End();
        for ( const AString * eIt=m_ExcludePaths.Begin(); eIt != eEnd; ++eIt )
        {
            if ( PathUtils::PathBeginsWith( it->m_Name, *eIt ) )
            {
                excluded = true;
                break;
            }
        }

        // filter excluded files
        if ( !excluded )
        {
            const AString * fit = m_FilesToExclude.Begin();
            const AString * const fend = m_FilesToExclude.End();
            for ( ; fit != fend; ++fit )
            {
                if ( PathUtils::PathEndsWithFile( it->m_Name, *fit ) )
                {
                    excluded = true;
                    break;
                }
            }
        }

        // filter excluded patterns
        if ( !excluded )
        {
            const AString * pit = m_ExcludePatterns.Begin();
            const AString * const pend = m_ExcludePatterns.End();
            for ( ; pit != pend; ++pit )
            {
                if ( PathUtils::IsWildcardMatch( pit->Get(), it->m_Name.Get() ) )
                {
                    excluded = true;
                    break;
                }
            }
        }

        if ( !excluded )
        {
            m_Files.Append( *it );
        }
    }

    MakePrettyName( files.GetSize() );

    if ( FLog::ShowInfo() )
    {
        const size_t numFiles = m_Files.GetSize();
        FLOG_INFO( "Dir: '%s' (found %u files)\n",
                            m_Name.Get(),
                            (uint32_t)numFiles);
        for ( size_t i=0; i<numFiles; ++i )
        {
            FLOG_INFO( " - %s\n", m_Files[ i ].m_Name.Get() );
        }
    }

    // Hash the directory listing to represent the discovered files
    // Note: We don't include any file attributes in this hash, because
    // if a downstream node depends on the files, it will do so directly
    // The hash only represents the list of discovered files
    if ( m_Files.IsEmpty() )
    {
        m_Stamp = 1; // Non-zero
    }
    else
    {
        MemoryStream ms;
        for ( const FileIO::FileInfo & file : m_Files )
        {
            ms.WriteBuffer( file.m_Name.Get(), file.m_Name.GetLength() );
        }
        m_Stamp = xxHash::Calc64( ms.GetData(), ms.GetSize() );
    }

    return NODE_RESULT_OK;
}

// MakePrettyName
//------------------------------------------------------------------------------
void DirectoryListNode::MakePrettyName( const size_t totalFiles )
{
    AStackString<> prettyName( m_Path );
    if (m_Recursive)
    {
        prettyName += " (recursive)";
    }

    const size_t numFiles = m_Files.GetSize();
    prettyName.AppendFormat( ", files kept: %zu / %zu", numFiles, totalFiles );

    m_PrettyName = prettyName;
}

//------------------------------------------------------------------------------

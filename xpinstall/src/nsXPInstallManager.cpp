/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, 
 * released March 31, 1998. 
 *
 * The Initial Developer of the Original Code is Netscape Communications 
 * Corporation.  Portions created by Netscape are 
 * Copyright (C) 1998-1999 Netscape Communications Corporation.  All Rights
 * Reserved.
 *
 * Contributors:
 *     Daniel Veditz <dveditz@netscape.com>
 */

#include "nscore.h"
#include "nsFileSpec.h"
#include "nsVector.h"

#include "nsISupports.h"
#include "nsIServiceManager.h"

#include "nsIURL.h"
#ifdef NECKO
#include "nsNeckoUtil.h"
#include "nsIBufferInputStream.h"
#else
#include "nsINetlibURL.h"
#include "nsINetService.h"
#endif
#include "nsIInputStream.h"
#include "nsIStreamListener.h"

#include "nsISoftwareUpdate.h"
#include "nsSoftwareUpdateIIDs.h"

#include "nsXPITriggerInfo.h"
#include "nsXPInstallManager.h"
#include "nsInstallProgressDialog.h"
#include "nsSpecialSystemDirectory.h"
#include "nsFileStream.h"
#include "nsProxyObjectManager.h"

#include "nsIAppShellComponentImpl.h"


static NS_DEFINE_IID(kISupportsIID, NS_ISUPPORTS_IID);
static NS_DEFINE_IID(kAppShellServiceCID, NS_APPSHELL_SERVICE_CID );
static NS_DEFINE_IID(kProxyObjectManagerCID, NS_PROXYEVENT_MANAGER_CID);

nsXPInstallManager::nsXPInstallManager()
  :  mTriggers(0), mItem(0), mNextItem(0)
{
    NS_INIT_ISUPPORTS();

    // we need to own ourself because we have a longer
    // lifetime than the scriptlet that created us.
    NS_ADDREF_THIS();
}



nsXPInstallManager::~nsXPInstallManager()
{    
    if (mTriggers) 
        delete mTriggers;
}


NS_IMPL_ADDREF( nsXPInstallManager );
NS_IMPL_RELEASE( nsXPInstallManager );

NS_IMETHODIMP 
nsXPInstallManager::QueryInterface(REFNSIID aIID,void** aInstancePtr)
{
  if (!aInstancePtr)
    return NS_ERROR_NULL_POINTER;

  if (aIID.Equals(nsIXPINotifier::GetIID()))
    *aInstancePtr = NS_STATIC_CAST(nsIXPINotifier*,this);
  else if (aIID.Equals(nsIStreamListener::GetIID()))
    *aInstancePtr = NS_STATIC_CAST(nsIStreamListener*,this);
  else if (aIID.Equals(kISupportsIID))
    *aInstancePtr = NS_STATIC_CAST( nsISupports*, NS_STATIC_CAST(nsIXPINotifier*,this));
  else
    *aInstancePtr = 0;

  nsresult status;
  if ( !*aInstancePtr )
    status = NS_NOINTERFACE;
  else 
  {
    NS_ADDREF_THIS();
    status = NS_OK;
  }

  return status;
}



NS_IMETHODIMP
nsXPInstallManager::InitManager(nsXPITriggerInfo* aTriggers)
{
    nsresult rv = NS_OK;
    mTriggers = aTriggers;

    if ( !mTriggers || mTriggers->Size() == 0 )
        rv = NS_ERROR_INVALID_POINTER;

    //
    // XXX Need modal confirmation dialog here...
    //

    // --- create and open the progress dialog
    if (NS_SUCCEEDED(rv))
    {
        nsInstallProgressDialog* dlg;
        nsCOMPtr<nsISupports>    Idlg;
        NS_NEWXPCOM( dlg, nsInstallProgressDialog );

        if ( dlg )
        {
            rv = dlg->QueryInterface( nsIXPIProgressDlg::GetIID(), getter_AddRefs(Idlg) );
            if (NS_SUCCEEDED(rv))
            {
                NS_WITH_SERVICE( nsIProxyObjectManager, pmgr, kProxyObjectManagerCID, &rv);
                if (NS_SUCCEEDED(rv))
                {
                    rv = pmgr->GetProxyObject( 0, nsIXPIProgressDlg::GetIID(),
                        Idlg, PROXY_SYNC, getter_AddRefs(mDlg) );

                    if (NS_SUCCEEDED(rv))
                        rv = mDlg->Open();
                }
            }
        }
        else
            rv = NS_ERROR_OUT_OF_MEMORY;
    }

    // --- start the first download (or clean up on error)
    if (NS_SUCCEEDED(rv))
        rv = DownloadNext();
    else 
        NS_RELEASE_THIS();

    return rv;
}


nsresult nsXPInstallManager::DownloadNext()
{
    nsresult rv;
    if ( mNextItem < mTriggers->Size() )
    {
        // download the next item in list

        mItem = (nsXPITriggerItem*)mTriggers->Get(mNextItem++);

        NS_ASSERTION( mItem, "bogus Trigger slipped through" );
        NS_ASSERTION( mItem->mURL.Length() > 0, "bogus trigger");
        if ( !mItem || mItem->mURL.Length() == 0 )
        {
            // serious problem with trigger! try to carry on
            rv = DownloadNext();
        }
        else if ( mItem->IsFileURL() )
        {
            // don't need to download, just point at local file
            rv = NS_NewFileSpecWithSpec( nsFileSpec(nsFileURL(mItem->mURL)),
                    getter_AddRefs(mItem->mFile) );
            if (NS_FAILED(rv))
                mItem->mFile = 0;

            rv = DownloadNext();
        }
        else
        {
            // We have one to download

            // --- figure out a temp file name
            nsSpecialSystemDirectory temp(nsSpecialSystemDirectory::OS_TemporaryDirectory);
            PRInt32 pos = mItem->mURL.RFind('/');
            if ( pos != -1 )
            {
                nsString jarleaf;
                mItem->mURL.Right( jarleaf, mItem->mURL.Length() - pos);
                temp += jarleaf;
            }
            else
                temp += "xpinstall.xpi";

            temp.MakeUnique();

            rv = NS_NewFileSpecWithSpec( temp, getter_AddRefs(mItem->mFile) );
            if (NS_SUCCEEDED(rv))
            {
                // --- start the download
                nsIURI  *pURL;
#ifdef NECKO
                rv = NS_NewURI(&pURL, mItem->mURL);
#else
                rv = NS_NewURL(&pURL, mItem->mURL);
#endif
                if (NS_SUCCEEDED(rv)) {
#ifdef NECKO
                    rv = NS_OpenURI( this, pURL );
#else
                    rv = NS_OpenURL( pURL, this );
#endif
                }
            }

            if (NS_FAILED(rv))
            {
                // XXX announce failure somehow
                mItem->mFile = 0;
                // carry on with the next one
                rv = DownloadNext();
            }
        }
    }
    else
    {
        NS_WITH_SERVICE(nsISoftwareUpdate, softupdate, nsSoftwareUpdate::GetCID(), &rv);
        // all downloaded, queue them for installation
        for (PRUint32 i = 0; i < mTriggers->Size(); ++i)
        {
            mItem = (nsXPITriggerItem*)mTriggers->Get(i);
            if ( mItem )
            {
                if (NS_SUCCEEDED(rv) && mItem->mFile )
                {
                    softupdate->InstallJar( mItem->mFile,
                                            mItem->mArguments.GetUnicode(),
                                            mItem->mFlags,
                                            this );
                }
                else
                    ; // XXX announce failure
            }
        }
    }

    return rv;
}





// IStreamListener methods
#ifndef NECKO
NS_IMETHODIMP
nsXPInstallManager::GetBindInfo(nsIURI* aURL, nsStreamBindingInfo* info)
{
  return NS_OK;
}

NS_IMETHODIMP
nsXPInstallManager::OnProgress( nsIURI* aURL,
                          PRUint32 Progress,
                          PRUint32 ProgressMax)
{
  return NS_OK;
}

NS_IMETHODIMP
nsXPInstallManager::OnStatus(nsIURI* aURL, 
                       const PRUnichar* aMsg)
{ 
    if (mDlg)
        return mDlg->SetActionText( aMsg );
    else
        return NS_ERROR_NULL_POINTER;
}
#endif

NS_IMETHODIMP
#ifdef NECKO
nsXPInstallManager::OnStartRequest(nsISupports *ctxt)
#else
nsXPInstallManager::OnStartRequest(nsIURI* aURL, 
                             const char *aContentType)
#endif
{
    mItem->mFile->openStreamForWriting();
    return NS_OK;
}

NS_IMETHODIMP
#ifdef NECKO
nsXPInstallManager::OnStopRequest(nsISupports *ctxt, nsresult status, 
                                  const PRUnichar *errorMsg)
#else
nsXPInstallManager::OnStopRequest(nsIURI* aURL,
                                        nsresult status,
                                        const PRUnichar* aMsg)
#endif
{
    nsresult rv;
    switch( status ) 
    {

        case NS_BINDING_SUCCEEDED:
            rv = NS_OK;
            break;

        case NS_BINDING_FAILED:
        case NS_BINDING_ABORTED:
            rv = status;
            // XXX need to note failure, both to send back status
            // to the callback, and also so we don't try to install
            // this probably corrupt file.
            break;

        default:
            rv = NS_ERROR_ILLEGAL_VALUE;
    }

    mItem->mFile->closeStream();
    DownloadNext();
    return rv;
}

#define BUF_SIZE 1024

NS_IMETHODIMP
#ifdef NECKO
nsXPInstallManager::OnDataAvailable(nsISupports *ctxt, 
                                    nsIInputStream *pIStream,
                                    PRUint32 sourceOffset, 
                                    PRUint32 length)
#else
nsXPInstallManager::OnDataAvailable(nsIURI* aURL,
                                    nsIInputStream *pIStream,
                                    PRUint32 length)
#endif
{
    PRUint32 len;
    PRInt32  result;
    nsresult err;
    char buffer[BUF_SIZE];
    
    do 
    {
        err = pIStream->Read(buffer, BUF_SIZE, &len);
        
        if (NS_SUCCEEDED(err))
        {
            err = mItem->mFile->write( buffer, len, &result);
            if ( NS_SUCCEEDED(err) && result != (PRInt32)len )
            {
                /* Error */ 
                err = NS_ERROR_FAILURE;
            }
        }

    } while (len > 0 && err == NS_OK);

    return err;
}




// IXPINotifier methods

NS_IMETHODIMP 
nsXPInstallManager::BeforeJavascriptEvaluation()
{
    mFinalizing = PR_FALSE;
    return NS_OK;
}

NS_IMETHODIMP 
nsXPInstallManager::AfterJavascriptEvaluation()
{
    return NS_OK;
}

NS_IMETHODIMP 
nsXPInstallManager::InstallStarted(const char *UIPackageName)
{
    return mDlg->SetHeading( nsString(UIPackageName).GetUnicode() );
}

NS_IMETHODIMP 
nsXPInstallManager::ItemScheduled(const char *message)
{
    PRBool cancelled = PR_FALSE;

    mDlg->GetCancelStatus(&cancelled);
    if (cancelled)
        return NS_ERROR_FAILURE;

    return mDlg->SetActionText( nsString(message).GetUnicode() );
}

NS_IMETHODIMP 
nsXPInstallManager::InstallFinalization(const char *message, PRInt32 itemNum, PRInt32 totNum)
{
    if (!mFinalizing)
    {
        mFinalizing = PR_TRUE;
        mDlg->SetActionText( nsString("Finishing install... please wait").GetUnicode() );
    }
    return mDlg->SetProgress( itemNum, totNum );
}

NS_IMETHODIMP 
nsXPInstallManager::InstallAborted()
{
    return NS_OK;
}

NS_IMETHODIMP 
nsXPInstallManager::LogComment(const char* comment)
{
    return NS_OK;
}


/*
 * Copyright (C) 2009 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "public/web/WebPageSerializer.h"

#include "core/HTMLNames.h"
#include "core/dom/Document.h"
#include "core/dom/Element.h"
#include "core/frame/Frame.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/RemoteFrame.h"
#include "core/html/HTMLAllCollection.h"
#include "core/html/HTMLFrameElementBase.h"
#include "core/html/HTMLFrameOwnerElement.h"
#include "core/html/HTMLInputElement.h"
#include "core/html/HTMLTableElement.h"
#include "core/loader/DocumentLoader.h"
#include "core/page/PageSerializer.h"
#include "platform/SerializedResource.h"
#include "platform/SharedBuffer.h"
#include "platform/mhtml/MHTMLArchive.h"
#include "platform/mhtml/MHTMLParser.h"
#include "platform/weborigin/KURL.h"
#include "public/platform/WebCString.h"
#include "public/platform/WebString.h"
#include "public/platform/WebURL.h"
#include "public/platform/WebVector.h"
#include "public/web/WebDocument.h"
#include "public/web/WebFrame.h"
#include "public/web/WebPageSerializerClient.h"
#include "web/WebLocalFrameImpl.h"
#include "web/WebPageSerializerImpl.h"
#include "web/WebRemoteFrameImpl.h"
#include "wtf/Assertions.h"
#include "wtf/HashMap.h"
#include "wtf/HashSet.h"
#include "wtf/Noncopyable.h"
#include "wtf/Vector.h"
#include "wtf/text/StringConcatenate.h"

namespace blink {

namespace {

using ContentIDMap = WillBeHeapHashMap<RawPtrWillBeMember<Frame>, String>;

class MHTMLPageSerializerDelegate final :
    public NoBaseWillBeGarbageCollected<MHTMLPageSerializerDelegate>,
    public PageSerializer::Delegate {
    WTF_MAKE_NONCOPYABLE(MHTMLPageSerializerDelegate);
public:
    MHTMLPageSerializerDelegate(const ContentIDMap& frameToContentID);
    bool shouldIgnoreAttribute(const Attribute&) override;
    bool rewriteLink(const Element&, String& rewrittenLink) override;

#if ENABLE(OILPAN)
    void trace(Visitor* visitor) { visitor->trace(m_frameToContentID); }
#endif

private:
    const ContentIDMap& m_frameToContentID;
};

MHTMLPageSerializerDelegate::MHTMLPageSerializerDelegate(
    const ContentIDMap& frameToContentID)
    : m_frameToContentID(frameToContentID)
{
}

bool MHTMLPageSerializerDelegate::shouldIgnoreAttribute(const Attribute& attribute)
{
    // TODO(fgorski): Presence of srcset attribute causes MHTML to not display images, as only the value of src
    // is pulled into the archive. Discarding srcset prevents the problem. Long term we should make sure to MHTML
    // plays nicely with srcset.
    return attribute.localName() == HTMLNames::srcsetAttr;
}

bool MHTMLPageSerializerDelegate::rewriteLink(
    const Element& element,
    String& rewrittenLink)
{
    if (!element.isFrameOwnerElement())
        return false;

    auto* frameOwnerElement = toHTMLFrameOwnerElement(&element);
    Frame* frame = frameOwnerElement->contentFrame();
    if (!frame)
        return false;

    KURL cidURI = MHTMLParser::convertContentIDToURI(m_frameToContentID.get(frame));
    ASSERT(cidURI.isValid());

    if (isHTMLFrameElementBase(&element)) {
        rewrittenLink = cidURI.string();
        return true;
    }

    if (isHTMLObjectElement(&element)) {
        Document* doc = frameOwnerElement->contentDocument();
        bool isHandledBySerializer = doc->isHTMLDocument()
            || doc->isXHTMLDocument() || doc->isImageDocument();
        if (isHandledBySerializer) {
            rewrittenLink = cidURI.string();
            return true;
        }
    }

    return false;
}

ContentIDMap createFrameToContentIDMap(
    const WebVector<std::pair<WebFrame*, WebString>>& webFrameToContentID)
{
    ContentIDMap result;
    for (const auto& it : webFrameToContentID) {
        WebFrame* webFrame = it.first;
        const WebString& webContentID = it.second;

        Frame* frame = webFrame->toImplBase()->frame();
        String contentID(webContentID);

        result.add(frame, contentID);
    }
    return result;
}

} // namespace

WebData WebPageSerializer::generateMHTMLHeader(
    const WebString& boundary, WebLocalFrame* frame)
{
    Document* document = toWebLocalFrameImpl(frame)->frame()->document();

    RefPtr<SharedBuffer> buffer = SharedBuffer::create();
    MHTMLArchive::generateMHTMLHeader(
        boundary, document->title(), document->suggestedMIMEType(),
        *buffer);
    return buffer.release();
}

WebData WebPageSerializer::generateMHTMLParts(
    const WebString& boundary, WebLocalFrame* webFrame, bool useBinaryEncoding,
    const WebVector<std::pair<WebFrame*, WebString>>& webFrameToContentID)
{
    // Translate arguments from public to internal blink APIs.
    LocalFrame* frame = toWebLocalFrameImpl(webFrame)->frame();
    MHTMLArchive::EncodingPolicy encodingPolicy = useBinaryEncoding
        ? MHTMLArchive::EncodingPolicy::UseBinaryEncoding
        : MHTMLArchive::EncodingPolicy::UseDefaultEncoding;
    ContentIDMap frameToContentID = createFrameToContentIDMap(webFrameToContentID);

    // Serialize.
    Vector<SerializedResource> resources;
    MHTMLPageSerializerDelegate delegate(frameToContentID);
    PageSerializer serializer(resources, &delegate);
    serializer.serializeFrame(*frame);

    // Encode serializer's output as MHTML.
    RefPtr<SharedBuffer> output = SharedBuffer::create();
    bool isFirstResource = true;
    for (const SerializedResource& resource : resources) {
        // Frame is the 1st resource (see PageSerializer::serializeFrame doc
        // comment). Frames need a Content-ID header.
        String contentID = isFirstResource ? frameToContentID.get(frame) : String();

        MHTMLArchive::generateMHTMLPart(
            boundary, contentID, encodingPolicy, resource, *output);

        isFirstResource = false;
    }
    return output.release();
}

WebData WebPageSerializer::generateMHTMLFooter(const WebString& boundary)
{
    RefPtr<SharedBuffer> buffer = SharedBuffer::create();
    MHTMLArchive::generateMHTMLFooter(boundary, *buffer);
    return buffer.release();
}

bool WebPageSerializer::serialize(
    WebLocalFrame* frame,
    WebPageSerializerClient* client,
    const WebVector<std::pair<WebURL, WebString>>& urlsToLocalPaths)
{
    WebPageSerializerImpl serializerImpl(frame, client, urlsToLocalPaths);
    return serializerImpl.serialize();
}

WebString WebPageSerializer::generateMetaCharsetDeclaration(const WebString& charset)
{
    // TODO(yosin) We should call |PageSerializer::metaCharsetDeclarationOf()|.
    String charsetString = "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=" + static_cast<const String&>(charset) + "\">";
    return charsetString;
}

WebString WebPageSerializer::generateMarkOfTheWebDeclaration(const WebURL& url)
{
    StringBuilder builder;
    builder.append("\n<!-- ");
    builder.append(PageSerializer::markOfTheWebDeclaration(url));
    builder.append(" -->\n");
    return builder.toString();
}

WebString WebPageSerializer::generateBaseTagDeclaration(const WebString& baseTarget)
{
    // TODO(yosin) We should call |PageSerializer::baseTagDeclarationOf()|.
    if (baseTarget.isEmpty())
        return String("<base href=\".\">");
    String baseString = "<base href=\".\" target=\"" + static_cast<const String&>(baseTarget) + "\">";
    return baseString;
}

} // namespace blink
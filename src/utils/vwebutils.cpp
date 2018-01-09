#include "vwebutils.h"

#include <QRegExp>
#include <QFileInfo>
#include <QDebug>
#include <QXmlStreamReader>
#include <QXmlStreamAttributes>

#include "vpalette.h"
#include "vconfigmanager.h"

extern VPalette *g_palette;

extern VConfigManager *g_config;

VWebUtils::VWebUtils()
{
}

void VWebUtils::init()
{
    m_stylesToRemoveWhenCopied = g_config->getStylesToRemoveWhenCopied();

    m_styleOfSpanForMark = g_config->getStyleOfSpanForMark();

    m_tagReg = QRegExp("<([^>/\\s]+)([^>]*)>");

    m_styleTagReg = QRegExp("<([^>\\s]+)([^>]*\\s)style=\"([^\">]+)\"([^>]*)>");

    initCopyTargets(g_config->getCopyTargets());
}

void VWebUtils::initCopyTargets(const QStringList &p_str)
{
    Q_ASSERT(m_copyTargets.isEmpty());
    // cap(1): action;
    // cap(3): arguments;
    QRegExp actReg("([0-9a-zA-Z])(\\(([^\\)]*)\\))?");

    for (auto const & str : p_str) {
        auto vals = str.split('$');
        if (vals.size() != 2) {
            continue;
        }

        CopyTarget tar;
        tar.m_name = vals[0];
        if (tar.m_name.isEmpty()) {
            continue;
        }

        auto acts = vals[1].split(':');
        for (auto const & it : acts) {
            if (it.isEmpty()) {
                continue;
            }

            if (!actReg.exactMatch(it)) {
                continue;
            }

            if (actReg.cap(1).size() != 1) {
                continue;
            }

            CopyTargetAction act;
            act.m_act = actReg.cap(1)[0];

            if (!actReg.cap(3).isEmpty()) {
                act.m_args = actReg.cap(3).toLower().split('|');
            }

            tar.m_actions.append(act);
        }

        m_copyTargets.append(tar);
    }

    qDebug() << "init" << m_copyTargets.size() << "copy targets";
}

bool VWebUtils::fixImageSrc(const QUrl &p_baseUrl, QString &p_html)
{
    bool changed = false;

#if defined(Q_OS_WIN)
    QUrl::ComponentFormattingOption strOpt = QUrl::EncodeSpaces;
#else
    QUrl::ComponentFormattingOption strOpt = QUrl::FullyEncoded;
#endif

    QRegExp reg("(<img src=\")([^\"]+)\"");

    int pos = 0;
    while (pos < p_html.size()) {
        int idx = p_html.indexOf(reg, pos);
        if (idx == -1) {
            break;
        }

        QString urlStr = reg.cap(2);
        QUrl imgUrl(urlStr);

        QString fixedStr;
        if (imgUrl.isRelative()) {
            fixedStr = p_baseUrl.resolved(imgUrl).toString(strOpt);
        } else if (imgUrl.isLocalFile()) {
            fixedStr = imgUrl.toString(strOpt);
        } else if (imgUrl.scheme() != "https" && imgUrl.scheme() != "http") {
            QString tmp = imgUrl.toString();
            if (QFileInfo::exists(tmp)) {
                fixedStr = QUrl::fromLocalFile(tmp).toString(strOpt);
            }
        }

        pos = idx + reg.matchedLength();
        if (!fixedStr.isEmpty() && urlStr != fixedStr) {
            qDebug() << "fix img url" << urlStr << fixedStr;
            // Insert one more space to avoid fix the url twice.
            pos = pos + fixedStr.size() + 1 - urlStr.size();
            p_html.replace(idx,
                           reg.matchedLength(),
                           QString("<img  src=\"%1\"").arg(fixedStr));
            changed = true;
        }
    }

    return changed;
}

QStringList VWebUtils::getCopyTargetsName() const
{
    QStringList names;
    for (auto const & it : m_copyTargets) {
        names << it.m_name;
    }

    return names;
}

bool VWebUtils::alterHtmlAsTarget(const QUrl &p_baseUrl, QString &p_html, const QString &p_target) const
{
    int idx = targetIndex(p_target);
    if (idx == -1) {
        return false;
    }

    bool altered = false;
    for (auto const & act : m_copyTargets[idx].m_actions) {
        if (const_cast<VWebUtils *>(this)->alterHtmlByTargetAction(p_baseUrl, p_html, act)) {
            altered = true;
        }
    }

    return altered;
}

int VWebUtils::targetIndex(const QString &p_target) const
{
    if (p_target.isEmpty()) {
        return -1;
    }

    for (int i = 0; i < m_copyTargets.size(); ++i) {
        if (m_copyTargets[i].m_name == p_target) {
            return i;
        }
    }

    return -1;
}

bool VWebUtils::alterHtmlByTargetAction(const QUrl &p_baseUrl, QString &p_html, const CopyTargetAction &p_action)
{
    bool altered = false;
    switch (p_action.m_act.toLatin1()) {
    case 's':
        if (!p_html.startsWith("<html>")) {
            p_html = "<html><body>" + p_html + "</body></html>";
            altered = true;
        }

        break;

    case 'b':
        altered = removeBackgroundColor(p_html, p_action.m_args);
        break;

    case 'c':
        altered = translateColors(p_html, p_action.m_args);
        break;

    case 'i':
        altered = fixImageSrc(p_baseUrl, p_html);
        break;

    case 'm':
        altered = removeMarginPadding(p_html, p_action.m_args);
        break;

    case 'x':
        altered = removeStylesToRemoveWhenCopied(p_html, p_action.m_args);
        break;

    case 'r':
        altered = removeAllStyles(p_html, p_action.m_args);
        break;

    case 'a':
        altered = transformMarkToSpan(p_html);
        break;

    case 'p':
        altered = replacePreBackgroundColorWithCode(p_html);
        break;

    default:
        break;
    }

    return altered;
}

static int skipToTagEnd(const QString &p_html, int p_pos, const QString &p_tag)
{
    QRegExp beginReg(QString("<%1 ").arg(p_tag));
    QRegExp endReg(QString("</%1>").arg(p_tag));

    int pos = p_pos;
    int nBegin = p_html.indexOf(beginReg, pos);
    int nEnd = p_html.indexOf(endReg, pos);
    if (nBegin > -1 && nBegin < nEnd) {
        // Nested tag.
        pos = skipToTagEnd(p_html, nBegin + beginReg.matchedLength(), p_tag);
        nEnd = p_html.indexOf(endReg, pos);
    }

    if (nEnd > -1) {
        pos = nEnd + endReg.matchedLength();
    }

    return pos;
}

// @p_html is the style string.
static bool removeStylesInStyleString(QString &p_html, const QStringList &p_styles)
{
    if (p_styles.isEmpty()) {
        return false;
    }

    int size = p_html.size();
    QRegExp reg(QString("(\\s|^)(%1):[^:]+;").arg(p_styles.join('|')));
    p_html.remove(reg);

    return size != p_html.size();
}

bool VWebUtils::removeBackgroundColor(QString &p_html, const QStringList &p_skipTags)
{
    QStringList styles({"background", "background-color"});

    return removeStyles(p_html, p_skipTags, styles);
}

bool VWebUtils::translateColors(QString &p_html, const QStringList &p_skipTags)
{
    bool changed = false;

    const QHash<QString, QString> &mapping = g_palette->getColorMapping();
    if (mapping.isEmpty()) {
        return changed;
    }

    // Won't mixed up with background-color.
    QRegExp colorReg("(\\s|^)color:([^;]+);");

    int pos = 0;
    while (pos < p_html.size()) {
        int tagIdx = p_html.indexOf(m_tagReg, pos);
        if (tagIdx == -1) {
            break;
        }

        QString tagName = m_tagReg.cap(1);
        if (p_skipTags.contains(tagName.toLower())) {
            // Skip this tag.
            pos = skipToTagEnd(p_html, tagIdx + m_tagReg.matchedLength(), tagName);
            continue;
        }

        pos = tagIdx;
        int idx = p_html.indexOf(m_styleTagReg, pos);
        if (idx == -1) {
            break;
        } else if (idx != tagIdx) {
            pos = tagIdx + m_tagReg.matchedLength();
            continue;
        }

        QString styleStr = m_styleTagReg.cap(3);
        QString alteredStyleStr = styleStr;
        int posb = 0;
        while (posb < alteredStyleStr.size()) {
            int idxb = alteredStyleStr.indexOf(colorReg, posb);
            if (idxb == -1) {
                break;
            }

            QString col = colorReg.cap(2).trimmed().toLower();
            auto it = mapping.find(col);
            if (it == mapping.end()) {
                posb = idxb + colorReg.matchedLength();
                continue;
            }

            // Replace the color.
            QString newCol = it.value();
            // Add one extra space between color and :.
            QString newStr = QString("%1color : %2;").arg(colorReg.cap(1)).arg(newCol);
            alteredStyleStr.replace(idxb, colorReg.matchedLength(), newStr);
            posb = idxb + newStr.size();
            changed = true;
        }

        if (changed) {
            QString newTag = QString("<%1%2style=\"%3\"%4>").arg(m_styleTagReg.cap(1))
                                                            .arg(m_styleTagReg.cap(2))
                                                            .arg(alteredStyleStr)
                                                            .arg(m_styleTagReg.cap(4));

            p_html.replace(idx, m_styleTagReg.matchedLength(), newTag);

            pos = idx + newTag.size();
        } else {
            pos = idx + m_styleTagReg.matchedLength();
        }
    }

    return changed;
}

bool VWebUtils::removeMarginPadding(QString &p_html, const QStringList &p_skipTags)
{
    QStringList styles({"margin", "margin-left", "margin-right",
                        "padding", "padding-left", "padding-right"});

    return removeStyles(p_html, p_skipTags, styles);
}

bool VWebUtils::removeStyles(QString &p_html, const QStringList &p_skipTags, const QStringList &p_styles)
{
    if (p_styles.isEmpty()) {
        return false;
    }

    bool altered = false;
    int pos = 0;

    while (pos < p_html.size()) {
        int tagIdx = p_html.indexOf(m_tagReg, pos);
        if (tagIdx == -1) {
            break;
        }

        QString tagName = m_tagReg.cap(1);
        if (p_skipTags.contains(tagName.toLower())) {
            // Skip this tag.
            pos = skipToTagEnd(p_html, tagIdx + m_tagReg.matchedLength(), tagName);
            continue;
        }

        pos = tagIdx;
        int idx = p_html.indexOf(m_styleTagReg, pos);
        if (idx == -1) {
            break;
        } else if (idx != tagIdx) {
            pos = tagIdx + m_tagReg.matchedLength();
            continue;
        }

        QString styleStr = m_styleTagReg.cap(3);
        if (removeStylesInStyleString(styleStr, p_styles)) {
            QString newTag = QString("<%1%2style=\"%3\"%4>").arg(m_styleTagReg.cap(1))
                                                            .arg(m_styleTagReg.cap(2))
                                                            .arg(styleStr)
                                                            .arg(m_styleTagReg.cap(4));
            p_html.replace(idx, m_styleTagReg.matchedLength(), newTag);

            pos = idx + newTag.size();

            altered = true;
        } else {
            pos = idx + m_styleTagReg.matchedLength();
        }
    }

    return altered;
}

bool VWebUtils::removeStylesToRemoveWhenCopied(QString &p_html, const QStringList &p_skipTags)
{
    return removeStyles(p_html, p_skipTags, m_stylesToRemoveWhenCopied);
}

bool VWebUtils::removeAllStyles(QString &p_html, const QStringList &p_skipTags)
{
    bool altered = false;
    int pos = 0;

    while (pos < p_html.size()) {
        int tagIdx = p_html.indexOf(m_tagReg, pos);
        if (tagIdx == -1) {
            break;
        }

        QString tagName = m_tagReg.cap(1);
        if (p_skipTags.contains(tagName.toLower())) {
            // Skip this tag.
            pos = skipToTagEnd(p_html, tagIdx + m_tagReg.matchedLength(), tagName);
            continue;
        }

        pos = tagIdx;
        int idx = p_html.indexOf(m_styleTagReg, pos);
        if (idx == -1) {
            break;
        } else if (idx != tagIdx) {
            pos = tagIdx + m_tagReg.matchedLength();
            continue;
        }

        QString newTag = QString("<%1%2%3>").arg(m_styleTagReg.cap(1))
                                            .arg(m_styleTagReg.cap(2))
                                            .arg(m_styleTagReg.cap(4));
        p_html.replace(idx, m_styleTagReg.matchedLength(), newTag);

        pos = idx + newTag.size();

        altered = true;
    }

    return altered;
}

bool VWebUtils::transformMarkToSpan(QString &p_html)
{
    bool altered = false;
    int pos = 0;

    while (pos < p_html.size()) {
        int tagIdx = p_html.indexOf(m_tagReg, pos);
        if (tagIdx == -1) {
            break;
        }

        QString tagName = m_tagReg.cap(1);
        if (tagName.toLower() != "mark") {
            pos = tagIdx + m_tagReg.matchedLength();
            continue;
        }

        pos = tagIdx;
        int idx = p_html.indexOf(m_styleTagReg, pos);
        if (idx == -1 || idx != tagIdx) {
            // <mark> without "style".
            QString newTag = QString("<span style=\"%1\" %2>").arg(m_styleOfSpanForMark)
                                                              .arg(m_tagReg.cap(2));
            p_html.replace(tagIdx, m_tagReg.matchedLength(), newTag);

            pos = tagIdx + newTag.size();

            altered = true;
            continue;
        }

        QString newTag = QString("<span%1style=\"%2\"%3>").arg(m_styleTagReg.cap(2))
                                                          .arg(m_styleTagReg.cap(3) + m_styleOfSpanForMark)
                                                          .arg(m_styleTagReg.cap(4));
        p_html.replace(idx, m_styleTagReg.matchedLength(), newTag);

        pos = idx + newTag.size();

        altered = true;
    }

    if (altered) {
        // Replace all </mark> with </span>.
        p_html.replace("</mark>", "</span>");
    }

    return altered;
}

bool VWebUtils::replacePreBackgroundColorWithCode(QString &p_html)
{
    if (p_html.isEmpty()) {
        return false;
    }

    bool altered = false;

    qDebug() << "html" << p_html;

    QXmlStreamReader xml(p_html);

    while (!xml.atEnd()) {
        qint64 lastOffset = xml.characterOffset();
        bool ret = xml.readNextStartElement();
        /*
        if (ret) {
            qDebug() << "start element" << xml.name();
        } else {
            QXmlStreamReader::TokenType tt = xml.readNext();
            if (tt == QXmlStreamReader::Invalid) {
                break;
            }

            switch (tt) {
            case QXmlStreamReader::StartElement:
                qDebug() << "start element" << xml.name();
                break;

            case QXmlStreamReader::EndElement:
                qDebug() << "end element" << xml.name();
                break;

            case QXmlStreamReader::Characters:
                qDebug() << "character element" << xml.text();
                break;

            default:
                break;
            }
        }
        */

        if (!ret) {
            lastOffset = xml.characterOffset();
            xml.readNext();
        }

        if (xml.hasError()) {
            break;
        }

        if (xml.isStartElement() && xml.name() == "pre") {
            QXmlStreamAttributes attr = xml.attributes();
            QStringRef preStyle = attr.value("style");
            qint64 offset = xml.characterOffset();
            qDebug() << "<pre>" << lastOffset << p_html.mid(lastOffset, 5) << offset << p_html.mid(offset, 5) << preStyle;
        }
    }

    return altered;
}

#pragma once

#include <QString>
#include <QStringList>
#include <functional>

QString selectNativeMenuForTest(const QStringList& path, const std::function<void()>& open);

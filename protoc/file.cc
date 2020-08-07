// Copyright 2016-2020 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#include "file.h"

#include <stddef.h>

#include <memory>
#include <set>

#include <google/protobuf/io/printer.h>
#include <google/protobuf/stubs/strutil.h>
#include "proto2-descriptor-extensions.pb.h"
#include "enum.h"
#include "field.h"
#include "message.h"
#include "names.h"
#include "service.h"

namespace google {
namespace protobuf {
namespace cl_protobufs {

// ===================================================================

FileGenerator::FileGenerator(const FileDescriptor* file) :
    file_(file),
    lisp_package_name_(FileLispPackage(file)),
    schema_name_(file->name()),
    enums_(file->enum_type_count()),
    messages_(file->message_type_count()),
    services_(file->service_count()) {
  for (int i = 0; i < file->enum_type_count(); ++i) {
    enums_[i] = std::make_unique<EnumGenerator>(file->enum_type(i));
  }

  for (int i = 0; i < file->message_type_count(); ++i) {
    messages_[i] = std::make_unique<MessageGenerator>(file->message_type(i));
  }

  for (int i = 0; i < file->service_count(); ++i) {
    services_[i] = std::make_unique<ServiceGenerator>(file->service(i));
  }

  switch (file_->syntax()) {
    case FileDescriptor::Syntax::SYNTAX_PROTO2:
      syntax_ = ":proto2";
      break;
    case FileDescriptor::Syntax::SYNTAX_PROTO3:
      syntax_ = ":proto3";
      break;
    case FileDescriptor::Syntax::SYNTAX_UNKNOWN:
      GOOGLE_LOG(FATAL) << "Unknown syntax for file: " << file->DebugString();
      break;
  }

  // Derive schema name.
  const size_t slash = schema_name_.find_last_of("\\/");
  if (std::string::npos != slash) {
    schema_name_.erase(0, slash + 1);
  }
  const size_t period = schema_name_.rfind('.');
  if (std::string::npos != period) {
    schema_name_.erase(period);
  }
  StrToLower(&schema_name_);
}

FileGenerator::~FileGenerator() {}

void FileGenerator::GenerateSource(io::Printer* printer) {
  printer->Print(
      ";;; $file_name$.lisp\n"
      ";;;\n"
      ";;; Generated by the protocol buffer compiler. DO NOT EDIT!\n",
      "file_name", file_->name());

  // Just in case multiple schema are written to the same file.
  printer->Print("\n(cl:in-package #:common-lisp-user)\n");

  std::set<std::string> packages;
  if (!lisp_package_name_.empty()) {
    packages.insert(lisp_package_name_);
    if (file_->service_count() > 0) {
      packages.insert(lisp_package_name_ + "-RPC");
    }
  }

  for (int i = 0; i < file_->message_type_count(); ++i) {
    messages_[i]->AddPackages(&packages);
  }

  printer->Print("\n#+sbcl (cl:declaim (cl:optimize (cl:debug 0)"
                 " (sb-c:store-coverage-data 0)))\n");
  for (const std::string& package : packages) {
    printer->Print(
        "\n(cl:eval-when (:compile-toplevel :load-toplevel :execute)\n"
        "  (cl:unless (cl:find-package \"$package_name$\")\n"
        "    (cl:defpackage \"$package_name$\" (:use))))\n",
        "package_name", package);
  }

  if (!lisp_package_name_.empty()) {
    printer->Print("\n(cl:in-package \"$package_name$\")\n",
                   "package_name", lisp_package_name_);
  }

  printer->Print(
      "\n(cl:eval-when (:compile-toplevel :load-toplevel :execute)"
      "\n(proto:define-schema '$schema_name$\n",
      "schema_name", schema_name_);
  printer->Indent();
  // Schema options.
  printer->Indent();
  const char* sep = "";
  printer->Print(":syntax $syntax$\n", "syntax", syntax_);
  if (!file_->package().empty()) {
    printer->Print(sep); sep = "\n ";
    printer->Print(":package \"$pck$\"", "pck", file_->package());
  }
  if (file_->dependency_count() > 0) {
    printer->Print(sep);
    printer->Print(":import '("); sep = "";
    for (int i = 0; i < file_->dependency_count(); ++i) {
      printer->Print(sep); sep = "\n          ";
      printer->Print("\"$import$\"", "import", file_->dependency(i)->name());
    }
    printer->Print(")"); sep = "\n ";
  }
  // END schema options
  printer->Print("))\n");
  printer->Outdent();
  printer->Outdent();

  std::vector<std::string> exports;
  exports.push_back(schema_name_);

  if (file_->enum_type_count() > 0) {
    printer->Print("\n;; Top-Level enums.");
    for (int i = 0; i < file_->enum_type_count(); ++i) {
      enums_[i]->Generate(printer);
      enums_[i]->AddExports(&exports);
    }
  }

  if (file_->message_type_count() > 0) {
    printer->Print("\n;; Top-Level messages.");
    for (int i = 0; i < file_->message_type_count(); ++i) {
      messages_[i]->Generate(printer);
      messages_[i]->AddExports(&exports);
    }
  }

  if (file_->extension_count() > 0) {
    printer->Print("\n;; Top-Level extensions.");
    for (int i = 0; i < file_->extension_count(); ++i) {
      GenerateExtension(printer, file_->extension(i), file_);
    }
  }

  std::vector<std::string> rpc_exports;
  if (file_->service_count() > 0) {
    printer->Print("\n;; Services.");
    for (int i = 0; i < file_->service_count(); ++i) {
      services_[i]->Generate(printer);
      services_[i]->AddExports(&exports);
      services_[i]->AddRpcExports(&rpc_exports);
    }
  }
  // END of schema defintion.

  // Register the schema by pathname.
  printer->Print(
      "\n\n"
      "(cl:eval-when (:compile-toplevel :load-toplevel :execute)\n"
      "(cl:setf (cl:gethash #P\"$file_name$\" proto-impl::*all-schemas*)\n"
      "         (proto:find-schema '$schema_name$)))\n",
      "file_name", file_->name(),
      "schema_name", schema_name_);

  if (!lisp_package_name_.empty()) {
    // Export symbols.
    if (!exports.empty()) {
      sep = "(";
      printer->Print("\n(cl:export '");
      for (const std::string& e : exports) {
        printer->Print(sep); sep = "\n             ";
        printer->PrintRaw(e);
      }
      printer->Print("))\n");
    }

    if (!rpc_exports.empty()) {
      printer->Print(
          "\n(cl:in-package \"$package_name$\")\n",
          "package_name", lisp_package_name_ + "-RPC");
      sep = "(";
      printer->Print("\n(cl:export '");
      for (const std::string& e : rpc_exports) {
        printer->Print(sep); sep = "\n             ";
        printer->PrintRaw(e);
      }
      printer->Print("))\n");
    }
  }
}

}  // namespace cl_protobufs
}  // namespace protobuf
}  // namespace google

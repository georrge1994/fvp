import 'dart:ffi';
import 'dart:io';
import 'generated_bindings.dart';

abstract class Libmdk {
  static DynamicLibrary _load() {
    String name;
    switch (Platform.operatingSystem) {
      case 'windows': name = 'mdk.dll';
      case 'macos': name = 'mdk.framework/mdk';
      case 'ios': name = 'mdk.framework/mdk';
      case 'linux': name ='libmdk.so.0';
      case 'android': name = 'libmdk.so';
      default:
        throw Exception(
          'Unsupported operating system: ${Platform.operatingSystem}.',
        );
    }
    try {
      return DynamicLibrary.open(name);
    } catch(e) {
      rethrow;
    }
  }

  static final instance = NativeLibrary(_load());
}
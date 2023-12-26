def main():
  import quisk
  if quisk.__file__.find('__init__') >= 0:	# quisk is the package
    import quisk.quisk as quisk
  quisk.main()

if __name__ == "__main__":
  main()
